// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sse_state — flux d'état versionné V10-P1 : producteur 30 Hz, ring
 * par client SSE, sysload. Déplacé tel quel (V14.0 étape 6).
 */
#include "gui_http.h"

/* ============================== SSE streaming ===================== */

/* E7.1 : callback chunked appelé en boucle par MHD pour pousser des frames
 * SSE meters au navigateur. usleep 33 ms entre frames = 30 Hz. Bloque le
 * worker MHD pendant le sleep — c'est OK avec MHD_THREAD_POOL=8.
 * Format RFC 8895 : `data: <json>\n\n` (double newline).
 */
ssize_t sse_stream_callback(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)cls; (void)pos;
	usleep(STREAM_PERIOD_US);

	/* E7.5 : meters reply now embeds the analyzer payload (4 taps × 128
	 * dB bins + 64 stereo scope pairs) so the buffer needs to grow past
	 * the previous 2 KB ceiling.
	 * V10-pre (critic f237748f) : buffer sur la PILE — le `static` était
	 * partagé entre les threads MHD (1 par client SSE) → corruption des
	 * trames dès 2 clients connectés (GUI PC + futur kiosk). */
	char meters_json[20480];
	int n = mixer_request("{\"op\":\"get_meters\"}\n", meters_json, sizeof(meters_json));
	if (n <= 0) {
		/* mixer-pro down : keep-alive comment frame pour que EventSource
		 * ne ferme pas la connexion (retry sera plus long sinon). */
		const char *keep = ": ka\n\n";
		size_t len = strlen(keep);
		if (len >= max) return MHD_CONTENT_READER_END_OF_STREAM;
		memcpy(buf, keep, len);
		return (ssize_t)len;
	}

	/* Strip trailing newline du JSON si présent (SSE va en ajouter 2) */
	if (n > 0 && meters_json[n-1] == '\n') meters_json[--n] = '\0';

	int len = snprintf(buf, max, "data: %s\n\n", meters_json);
	if (len < 0 || (size_t)len >= max) return MHD_CONTENT_READER_END_OF_STREAM;
	return len;
}


/* ================== V10-P1 : /api/state — flux d'état versionné ==========
 * ARCHI V10 v3.1 (annexes A-C) :
 *  - UN thread producteur poll mixer-pro (chaud 5 Hz : insert/assistant/stat,
 *    froid 1 Hz : input_map/output_gain + sysload calculé ici même) et émet :
 *      · patch par SECTION ENTIÈRE quand elle change :
 *        data: {"seq":N,"patch":{"insert":{...}}}
 *      · full state toutes les 30 s (filet) et à chaque nouveau client :
 *        data: {"seq":N,"full":{...toutes les sections...}}
 *  - par client : ring de trames, drop-oldest (le producteur ne bloque
 *    jamais), réveil par cond var (pas de mixer_request par client).
 *  - slot 0 réservé au kiosk (?panel=1) — jamais 503 pour l'écran local.
 *  - le producteur est LA source sysload (le handler /api/sysload sert son
 *    cache : plus de double fenêtre de mesure — finding critic it.2).
 * Les FX bus (fx0-3, potentiellement ~40 Ko avec meta) ne sont PAS dans le
 * flux : la page EFFETS les charge par POST get_fx à l'ouverture (P2). */

/* SC_* + struct sclient : gui_http.h (V14.0 étape 6) */
struct sclient g_sc[SC_MAX];
char g_sc_ring[SC_MAX][SC_RING][SC_FRAME];   /* 1.5 MB BSS, statique */
static pthread_mutex_t g_sc_alloc_mu = PTHREAD_MUTEX_INITIALIZER;
unsigned g_sseq = 0;
unsigned long g_sc_frames_out = 0;

char g_sysload_json[256] = "";
pthread_mutex_t g_sysload_mu = PTHREAD_MUTEX_INITIALIZER;

/* sections du state (hors sysload, géré à part) */
struct ssec {
	const char *name, *req;
	int hot;                       /* 1 = poll 5 Hz, 0 = 1 Hz */
	char cache[SC_FRAME/4];        /* 8 KB par section, insert le + gros */
};
static struct ssec g_secs[] = {
	{ "insert",      "{\"op\":\"get_insert\"}\n",      1, "" },
	{ "assistant",   "{\"op\":\"get_assistant\"}\n",   1, "" },
	{ "stat",        "{\"op\":\"get_state\"}\n",       1, "" },
	{ "input_map",   "{\"op\":\"get_input_map\"}\n",   0, "" },
	{ "output_gain", "{\"op\":\"get_output_gain\"}\n", 0, "" },
};
#define N_SECS ((int)(sizeof(g_secs)/sizeof(g_secs[0])))

static void sc_push(struct sclient *c, const char *frame, int len)
{
	if (len <= 0 || len >= SC_FRAME) return;
	pthread_mutex_lock(&c->mu);
	if (c->used) {
		if (c->wr - c->rd >= SC_RING) { c->rd++; c->drops++; }
		int slot = c->wr % SC_RING;
		memcpy(g_sc_ring[c - g_sc][slot], frame, len);
		c->lens[slot] = len;
		c->wr++;
		pthread_cond_signal(&c->cv);
	}
	pthread_mutex_unlock(&c->mu);
}

void sc_broadcast(const char *frame, int len)
{
	for (int i = 0; i < SC_MAX; i++)
		if (g_sc[i].used) sc_push(&g_sc[i], frame, len);
	g_sc_frames_out++;
}

/* construit la trame full depuis les caches ; retourne la longueur */
int sc_build_full(char *out, size_t sz)
{
	int n = snprintf(out, sz, "data: {\"schema_version\":1,\"seq\":%u,\"full\":{",
	                 ++g_sseq);
	for (int s = 0; s < N_SECS; s++)
		n += snprintf(out + n, sz - n, "%s\"%s\":%s",
		              s ? "," : "", g_secs[s].name,
		              g_secs[s].cache[0] ? g_secs[s].cache : "null");
	pthread_mutex_lock(&g_sysload_mu);
	n += snprintf(out + n, sz - n, ",\"sysload\":%s",
	              g_sysload_json[0] ? g_sysload_json : "null");
	pthread_mutex_unlock(&g_sysload_mu);
	n += snprintf(out + n, sz - n, "}}\n\n");
	return (n > 0 && n < (int)sz) ? n : 0;
}

/* --- sysload : calcul unique (déplacé du handler, source unique) --- */
static void compute_sysload(void)
{
	static unsigned long long prev_busy[4], prev_total[4];
	int cpu_pct[4] = {0, 0, 0, 0};
	FILE *f = fopen("/proc/stat", "r");
	if (f) {
		char ln[256];
		while (fgets(ln, sizeof(ln), f)) {
			int c;
			unsigned long long u, ni, s, idle, iow, irq, sirq, st;
			if (sscanf(ln, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
				   &c, &u, &ni, &s, &idle, &iow, &irq, &sirq, &st) == 9
			    && c >= 0 && c < 4) {
				unsigned long long busy = u + ni + s + irq + sirq + st;
				unsigned long long total = busy + idle + iow;
				unsigned long long db = busy - prev_busy[c];
				unsigned long long dt = total - prev_total[c];
				if (prev_total[c] && dt > 0)
					cpu_pct[c] = (int)(db * 100 / dt);
				prev_busy[c] = busy;
				prev_total[c] = total;
			}
		}
		fclose(f);
	}
	static unsigned int prev_beat;
	static int dsp_pct = -1;
	f = fopen("/sys/kernel/debug/sof/debug", "rb");
	if (f) {
		unsigned int regs[2] = {0, 0};
		if (fseek(f, 0xE0, SEEK_SET) == 0 && fread(regs, 4, 2, f) == 2) {
			dsp_pct = (regs[1] != prev_beat && regs[0] <= 100)
				  ? (int)regs[0] : 0;
			prev_beat = regs[1];
		}
		fclose(f);
	}
	/* C2 : charge réelle audio_thread depuis la section stat déjà pollée
	 * (pas de mixer_request supplémentaire) */
	{
		const char *st = g_secs[2].cache;
		long mix_us = 0, play_us = 0;
		const char *p = strstr(st, "\"prof_mix_us\":");
		if (p) mix_us = atol(p + 14);
		p = strstr(st, "\"prof_play_us\":");
		if (p) play_us = atol(p + 15);
		if (mix_us > 0) {
			int c2 = (int)((mix_us + play_us) / 20);
			cpu_pct[2] = c2 > 100 ? 100 : c2;
		}
	}
	int gpu = -1, npu = -1;
	f = fopen("/sys/kernel/debug/gc/load", "r");
	if (f) {
		char ln[128];
		int core = -1;
		while (fgets(ln, sizeof(ln), f)) {
			int v;
			if (sscanf(ln, "core : %d", &v) == 1) core = v;
			else if (sscanf(ln, "load : %d%%", &v) == 1) {
				if (core == 0) gpu = v;
				else if (core == 1) npu = v;
			}
		}
		fclose(f);
	}
	pthread_mutex_lock(&g_sysload_mu);
	snprintf(g_sysload_json, sizeof(g_sysload_json),
		 "{\"ok\":true,\"cpu\":[%d,%d,%d,%d],\"gpu\":%d,\"npu\":%d,\"dsp\":%d}",
		 cpu_pct[0], cpu_pct[1], cpu_pct[2], cpu_pct[3], gpu, npu, dsp_pct);
	pthread_mutex_unlock(&g_sysload_mu);
}

void *state_producer(void *arg)
{
	(void)arg;
	char resp[SC_FRAME/4], frame[SC_FRAME];
	int tick = 0;
	for (;;) {
		usleep(200000);              /* 5 Hz de base */
		tick++;
		/* V10-N8 : AUCUN client SSE → aucun poll mixer-pro (le control
		 * thread vit sur les cores audio isolés ; le poller tournait à
		 * vide en usage console pure). need_full ré-amorce à la
		 * connexion suivante. */
		int any = 0;
		for (int i = 0; i < SC_MAX; i++)
			if (g_sc[i].used) { any = 1; break; }
		if (!any)
			continue;
		int cold = (tick % 5) == 0;  /* 1 Hz */
		int full = (tick % 150) == 0;/* 30 s */

		for (int s = 0; s < N_SECS; s++) {
			if (!g_secs[s].hot && !cold && !full) continue;
			int n = mixer_request(g_secs[s].req, resp, sizeof(resp));
			if (n <= 0) continue;
			if (resp[n-1] == '\n') resp[--n] = '\0';
			if (strcmp(resp, g_secs[s].cache) != 0) {
				strncpy(g_secs[s].cache, resp, sizeof(g_secs[s].cache) - 1);
				if (!full) {   /* patch section entière */
					int fl = snprintf(frame, sizeof(frame),
						"data: {\"seq\":%u,\"patch\":{\"%s\":%s}}\n\n",
						++g_sseq, g_secs[s].name, resp);
					if (fl > 0 && fl < (int)sizeof(frame))
						sc_broadcast(frame, fl);
				}
			}
		}
		if (cold) {
			compute_sysload();
			/* sysload change quasi toujours → patch dédié 1 Hz */
			pthread_mutex_lock(&g_sysload_mu);
			int fl = snprintf(frame, sizeof(frame),
				"data: {\"seq\":%u,\"patch\":{\"sysload\":%s}}\n\n",
				++g_sseq, g_sysload_json);
			pthread_mutex_unlock(&g_sysload_mu);
			if (fl > 0 && fl < (int)sizeof(frame))
				sc_broadcast(frame, fl);
		}
		/* full périodique OU demandé par de nouveaux clients */
		int need = full;
		for (int i = 0; i < SC_MAX && !need; i++)
			if (g_sc[i].used && g_sc[i].need_full) need = 1;
		if (need) {
			int fl = sc_build_full(frame, sizeof(frame));
			if (fl > 0) {
				if (full) sc_broadcast(frame, fl);
				else for (int i = 0; i < SC_MAX; i++)
					if (g_sc[i].used && g_sc[i].need_full) {
						sc_push(&g_sc[i], frame, fl);
						g_sc[i].need_full = 0;
					}
				if (full)
					for (int i = 0; i < SC_MAX; i++) g_sc[i].need_full = 0;
			}
		}
	}
	return NULL;
}

/* --- callback SSE state : consomme le ring, réveillé par le producteur --- */
ssize_t sse_state_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)pos;
	struct sclient *c = cls;
	pthread_mutex_lock(&c->mu);
	if (c->rd == c->wr) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 500000000L;
		if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		pthread_cond_timedwait(&c->cv, &c->mu, &ts);
	}
	if (c->rd == c->wr) {          /* timeout → keep-alive */
		pthread_mutex_unlock(&c->mu);
		if (max < 6) return 0;
		memcpy(buf, ": ka\n\n", 6);
		return 6;
	}
	int slot = c->rd % SC_RING;
	int len = c->lens[slot];
	if ((size_t)len > max) len = (int)max;
	memcpy(buf, g_sc_ring[c - g_sc][slot], len);
	c->rd++;
	pthread_mutex_unlock(&c->mu);
	return len;
}

void sse_state_free(void *cls)
{
	struct sclient *c = cls;
	pthread_mutex_lock(&g_sc_alloc_mu);
	pthread_mutex_lock(&c->mu);
	c->used = 0;
	pthread_mutex_unlock(&c->mu);
	pthread_mutex_unlock(&g_sc_alloc_mu);
}

struct sclient *sc_alloc(int is_panel)
{
	pthread_mutex_lock(&g_sc_alloc_mu);
	int lo = is_panel ? 0 : 1, hi = is_panel ? 1 : SC_MAX;
	struct sclient *c = NULL;
	for (int i = lo; i < hi; i++)
		if (!g_sc[i].used) { c = &g_sc[i]; break; }
	if (c) {
		c->used = 1; c->is_panel = is_panel;
		c->wr = c->rd = 0; c->drops = 0; c->need_full = 1;
	}
	pthread_mutex_unlock(&g_sc_alloc_mu);
	return c;
}

