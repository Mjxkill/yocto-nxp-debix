// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sampler — V12-SMP : sampleur one-shot (voir sampler.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 1, extraction pure).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "state.h"     /* g_st.target_lock */
#include "util.h"      /* mlog */
#include "sampler.h"
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <unistd.h>

#define SMP_MAX_TOTAL (256u * 1024u * 1024u)   /* plafond RAM (critic) */

struct smp_slot g_smp[SMP_SLOTS];
static float *g_smp_defer[SMP_SLOTS];
static int g_smp_defer_n;
static size_t g_smp_total;

/* Parseur WAV minimal : PCM 16/24/32 ou float32, mono→dup ou stéréo,
 * 48 kHz exigé. Retourne buffer float stéréo malloc'é (control thread). */
static float *smp_load_wav(const char *path, uint32_t *out_frames)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t h[12];
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) ||
	    memcmp(h + 8, "WAVE", 4)) {
		fclose(f);
		return NULL;
	}
	uint16_t fmt = 0, ch = 0, bits = 0;
	uint32_t rate = 0, data_len = 0;
	long data_off = -1;
	uint8_t ck[8];
	while (fread(ck, 1, 8, f) == 8) {
		uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			uint8_t b[16];
			if (len < 16 || fread(b, 1, 16, f) != 16)
				break;
			fmt  = b[0] | b[1] << 8;
			ch   = b[2] | b[3] << 8;
			rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
			bits = b[14] | b[15] << 8;
			if (len > 16)
				fseek(f, len - 16, SEEK_CUR);
		} else if (!memcmp(ck, "data", 4)) {
			data_off = ftell(f);
			data_len = len;
			fseek(f, (len + 1) & ~1u, SEEK_CUR);
		} else {
			fseek(f, (len + 1) & ~1u, SEEK_CUR);
		}
	}
	if (data_off < 0 || rate != 48000 || ch < 1 || ch > 2 ||
	    !((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
	      (fmt == 3 && bits == 32))) {
		mlog("smp: %s rejeté (fmt=%u ch=%u rate=%u bits=%u — 48k PCM/f32 requis)",
		     path, fmt, ch, rate, bits);
		fclose(f);
		return NULL;
	}
	const uint32_t bpf = ch * bits / 8;
	uint32_t frames = data_len / bpf;
	if ((size_t)frames * 8 + g_smp_total > SMP_MAX_TOTAL) {
		mlog("smp: %s rejeté (plafond RAM %u Mo atteint)",
		     path, SMP_MAX_TOTAL >> 20);
		fclose(f);
		return NULL;
	}
	uint8_t *raw = malloc(data_len);
	float *out = malloc((size_t)frames * 2 * sizeof(float));
	if (!raw || !out) {
		free(raw); free(out); fclose(f);
		return NULL;
	}
	fseek(f, data_off, SEEK_SET);
	if (fread(raw, 1, data_len, f) != data_len) {
		free(raw); free(out); fclose(f);
		return NULL;
	}
	fclose(f);
	for (uint32_t i = 0; i < frames; i++) {
		float l = 0, r = 0;
		for (int c = 0; c < ch; c++) {
			const uint8_t *p = raw + (size_t)i * bpf + c * bits / 8;
			float v;
			if (fmt == 3) {
				memcpy(&v, p, 4);
			} else if (bits == 16) {
				v = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
			} else if (bits == 24) {
				int32_t s = (p[0] << 8 | p[1] << 16 |
					     (uint32_t)p[2] << 24);
				v = (s >> 8) / 8388608.0f;
			} else {
				int32_t s = p[0] | p[1] << 8 | p[2] << 16 |
					    (uint32_t)p[3] << 24;
				v = s / 2147483648.0f;
			}
			if (c == 0) l = v;
			r = v;
		}
		if (ch == 1)
			r = l;
		out[i * 2] = l;
		out[i * 2 + 1] = r;
	}
	free(raw);
	*out_frames = frames;
	return out;
}

/* Scan du répertoire (tri alpha → slots). Appelé au démarrage (avant
 * threads) et par sampler_reload (control thread, publie sous lock). */
static int smp_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void smp_scan(int locked)
{
	DIR *d = opendir(SMP_DIR);
	char *names[128];
	int n = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) && n < 128) {
			size_t l = strlen(e->d_name);
			if (l > 4 && !strcasecmp(e->d_name + l - 4, ".wav"))
				names[n++] = strdup(e->d_name);
		}
		closedir(d);
	}
	qsort(names, n, sizeof(char *), smp_name_cmp);

	/* charge hors lock (IO + malloc), publie sous lock */
	float *bufs[SMP_SLOTS] = {0};
	uint32_t frs[SMP_SLOTS] = {0};
	char nms[SMP_SLOTS][64] = {{0}};
	g_smp_total = 0;
	for (int i = 0; i < n && i < SMP_SLOTS; i++) {
		char path[512];
		snprintf(path, sizeof(path), SMP_DIR "/%s", names[i]);
		bufs[i] = smp_load_wav(path, &frs[i]);
		if (bufs[i]) {
			g_smp_total += (size_t)frs[i] * 8;
			snprintf(nms[i], sizeof(nms[i]), "%s", names[i]);
			nms[i][strcspn(nms[i], ".")] = 0;   /* sans extension */
		}
	}
	for (int i = 0; i < n; i++)
		free(names[i]);

	if (locked)
		pthread_mutex_lock(&g_st.target_lock);
	/* purge différée du round PRÉCÉDENT (plus personne ne les lit) */
	for (int i = 0; i < g_smp_defer_n; i++)
		free(g_smp_defer[i]);
	g_smp_defer_n = 0;
	for (int i = 0; i < SMP_SLOTS; i++) {
		atomic_store(&g_smp[i].playing, 0);
		atomic_store(&g_smp[i].pos, 0);
		if (g_smp[i].buf)
			g_smp_defer[g_smp_defer_n++] = g_smp[i].buf;
		g_smp[i].buf = bufs[i];
		g_smp[i].frames = frs[i];
		g_smp[i].gain = 1.0f;
		snprintf(g_smp[i].name, sizeof(g_smp[i].name), "%s", nms[i]);
	}
	if (locked)
		pthread_mutex_unlock(&g_st.target_lock);
	int loaded = 0;
	for (int i = 0; i < SMP_SLOTS; i++)
		if (g_smp[i].buf)
			loaded++;
	mlog("smp: %d samples chargés (%zu Ko)", loaded, g_smp_total >> 10);
}

/* Rendu (audio_thread, SOUS target_lock, après le convert S32→float) */
void smp_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	const int L = N_INPUT_MICS + N_INPUT_STEMS;     /* P1 = 16 */
	for (int sl = 0; sl < SMP_SLOTS; sl++) {
		struct smp_slot *s = &g_smp[sl];
		if (!atomic_load_explicit(&s->playing, memory_order_acquire))
			continue;
		const float *b = s->buf;
		if (!b) {
			atomic_store(&s->playing, 0);
			continue;
		}
		uint32_t pos = atomic_load_explicit(&s->pos,
						    memory_order_relaxed);
		uint32_t left = s->frames > pos ? s->frames - pos : 0;
		uint32_t n = left < PERIOD_FRAMES ? left : PERIOD_FRAMES;
		const float g = s->gain;
		for (uint32_t f = 0; f < n; f++) {
			in_block[L][f]     += b[(size_t)(pos + f) * 2] * g;
			in_block[L + 1][f] += b[(size_t)(pos + f) * 2 + 1] * g;
		}
		pos += n;
		if (pos >= s->frames) {
			atomic_store(&s->playing, 0);
			atomic_store(&s->pos, 0);
		} else {
			atomic_store_explicit(&s->pos, pos,
					      memory_order_relaxed);
		}
	}
}

/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int sampler_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "sampler_list")) {
		/* V12-SMP : slots (nom, durée s, playing, position s) */
		int n = snprintf(g_ctl_reply, sizeof(g_ctl_reply), "{\"ok\":true,\"slots\":[");
		for (int i = 0; i < SMP_SLOTS; i++) {
			struct smp_slot *s = &g_smp[i];
			n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n,
				"%s{\"slot\":%d,\"name\":\"%s\",\"len_s\":%.1f,"
				"\"playing\":%d,\"pos_s\":%.1f}",
				i ? "," : "", i, s->buf ? s->name : "",
				s->frames / 48000.0f,
				atomic_load(&s->playing),
				atomic_load(&s->pos) / 48000.0f);
		}
		n += snprintf(g_ctl_reply + n, sizeof(g_ctl_reply) - n, "]}\n");
		write(fd, g_ctl_reply, n);
		return 1;
	}
	if (json_has_op(line, "sampler_trigger")) {
		/* {"op":"sampler_trigger","slot":N,"gain_db":F} — retrigger OK */
		int slot;
		float gdb = 0.0f;
		if (json_get_int(line, "slot", &slot) < 0 ||
		    slot < 0 || slot >= SMP_SLOTS || !g_smp[slot].buf) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad slot\"}\n");
			return 1;
		}
		(void)json_get_float(line, "gain_db", &gdb);
		g_smp[slot].gain = powf(10.0f, gdb / 20.0f);
		atomic_store(&g_smp[slot].pos, 0);
		atomic_store_explicit(&g_smp[slot].playing, 1,
				      memory_order_release);
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_trigger\",\"slot\":%d}\n",
			slot);
		return 1;
	}
	if (json_has_op(line, "sampler_stop")) {
		/* {"op":"sampler_stop","slot":N|-1} — -1 = tous */
		int slot = -1;
		(void)json_get_int(line, "slot", &slot);
		for (int i = 0; i < SMP_SLOTS; i++)
			if (slot < 0 || slot == i)
				atomic_store(&g_smp[i].playing, 0);
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_stop\"}\n");
		return 1;
	}
	if (json_has_op(line, "sampler_reload")) {
		smp_scan(1);
		int loaded = 0;
		for (int i = 0; i < SMP_SLOTS; i++)
			if (g_smp[i].buf)
				loaded++;
		dprintf(fd, "{\"ok\":true,\"op\":\"sampler_reload\",\"loaded\":%d}\n",
			loaded);
		return 1;
	}
	return 0;
}
