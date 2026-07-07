/* SPDX-License-Identifier: MIT
 *
 * V12-MIDIX — Expandeur MIDI (module de sons) pour la console A.L.A.
 *
 * Daemon séparé (cores 0-1, jamais sur les cores RT du mixer) :
 *   MIDI in (port f_midi du gadget USB) → libfluidsynth (SoundFont GM)
 *   → rendu stéréo 48 kHz par blocs de 96 frames (2 ms) dans un ring SHM
 *   lock-free SPSC → consommé par l'audio_thread de mixer-pro (tranches
 *   P1/P2). ARCHI_V12_MIDI_EXPANDER.md.
 *
 * Le daemon EST l'horloge du rendu (clock_nanosleep absolu 2 ms) ; le
 * consumer se resynchronise en cas de dérive. Pas de reverb/chorus fluid
 * (la console a ses propres FX). Config : /etc/ala/midi-expander.conf.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fluidsynth.h>
#include "ala-synth.h"

#define SAMPLE_RATE   48000
#define PERIOD_FRAMES 96
#define RING_PERIODS  256                      /* ≈ 0,5 s de marge */
#define RING_FRAMES   (RING_PERIODS * PERIOD_FRAMES)
#define MIDIX_SHM     "/ala-midix"
#define MIDIX_MAGIC   0x4D494458u              /* 'MIDX' */
#define CONF_PATH     "/etc/ala/midi-expander.conf"
#define SF2_DEFAULT   "/usr/share/sounds/sf2/GeneralUserGS.sf2"
#define CTL_SOCK      "/run/midi-expander.sock"
#define CHANS_CONF    "/var/lib/ala/midix-chans.conf"

struct midix_hdr {
	uint32_t magic;
	uint32_t ring_frames;
	_Atomic uint32_t widx;      /* frames écrites, free-running */
	uint32_t _pad;
};

static volatile sig_atomic_t g_run = 1;
static void on_term(int s) { (void)s; g_run = 0; }

static void mlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ========== V12-MIDIX-GUI — contrôle GUI + persistance par canal ==========
 * Socket texte /run/midi-expander.sock (une requête = une ligne, une
 * réponse JSON = une ligne). Servi par un thread dédié — la boucle de
 * rendu 2 ms n'est jamais touchée (l'API fluid_synth_* est thread-safe).
 * Protocole :  status | prog <chan> <num> | gain <val> | panic
 * Persistance /var/lib/ala/midix-chans.conf (débounce 2 s). */
static fluid_synth_t *g_synth;
static char g_sf2_name[512];
static _Atomic int g_chans_dirty;

/* V12-VU — activité MIDI par canal (milli-unités 0-1000). Alimentée par
 * le callback MIDI (note-on → vélocité), retombée exponentielle dans la
 * boucle de rendu (~500 ms). Affichage GUI (races bénignes). */
static _Atomic uint32_t g_act[16];

static int midi_ev_cb(void *data, fluid_midi_event_t *ev)
{
	int type = fluid_midi_event_get_type(ev);
	int chan = fluid_midi_event_get_channel(ev);
	if (type == 0x90) {   /* note-on */
		int vel = fluid_midi_event_get_velocity(ev);
		if (chan >= 0 && chan < 16 && vel > 0) {
			uint32_t v = (uint32_t)(vel * 1000 / 127);
			if (v > atomic_load_explicit(&g_act[chan],
						     memory_order_relaxed))
				atomic_store_explicit(&g_act[chan], v,
						      memory_order_relaxed);
		}
	}
	/* V12-SYNTH : les canaux M1 consomment leurs notes/CC (file SPSC
	 * vers le rendu) — surtout NE PAS les relayer à fluid (double son) */
	if (sy_chan_is_m1(chan) &&
	    (type == 0x90 || type == 0x80 || type == 0xB0)) {
		int d1 = fluid_midi_event_get_key(ev);
		int d2 = fluid_midi_event_get_velocity(ev);
		if (type == 0xB0) {
			d1 = fluid_midi_event_get_control(ev);
			d2 = fluid_midi_event_get_value(ev);
		}
		sy_midi(type, chan, d1, d2);
		return FLUID_OK;
	}
	return fluid_synth_handle_midi_event(data, ev);
}

static void chans_save(void)
{
	char tmp[sizeof(CHANS_CONF) + 4];
	snprintf(tmp, sizeof(tmp), "%s.tmp", CHANS_CONF);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "gain %.3f\n", fluid_synth_get_gain(g_synth));
	for (int c = 0; c < 16; c++) {
		int sf, bank, prog;
		if (fluid_synth_get_program(g_synth, c, &sf, &bank,
					    &prog) == FLUID_OK)
			fprintf(f, "chan %d %d\n", c, prog);
	}
	sy_save_chans(f);   /* V12-SYNTH : assignations moteur/patch */
	fclose(f);
	rename(tmp, CHANS_CONF);
}

static void chans_load(void)
{
	FILE *f = fopen(CHANS_CONF, "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		int c, p;
		float g;
		if (sscanf(line, "gain %f", &g) == 1 && g >= 0.0f && g <= 10.0f)
			fluid_synth_set_gain(g_synth, g);
		else if (sscanf(line, "chan %d %d", &c, &p) == 2 &&
			 c >= 0 && c < 16 && p >= 0 && p < 128)
			fluid_synth_program_change(g_synth, c, p);
		else
			sy_load_chan_line(line);   /* V12-SYNTH : engine c e p */
	}
	fclose(f);
	mlog("midix: programmes par canal restaurés");
}

static void ctl_handle(int fd, const char *req)
{
	static char out[16384];   /* inst_list : ~250 noms — thread ctl unique */
	int chan, num;
	float val;

	/* V13-SCENES E2 : rappel de scène — recharge patches + canaux
	 * (les fichiers ont été remplacés par gui-http) */
	if (!strncmp(req, "reload", 6)) {
		sy_reload_patches();
		chans_load();
		(void)!write(fd, "{\"ok\":true,\"op\":\"reload\"}\n", 26);
		return;
	}

	/* V12-SYNTH : engine / inst_list / patch_* traités par le moteur */
	if (sy_ctl(req, out, sizeof(out))) {
		if (!strncmp(req, "engine ", 7))
			atomic_store(&g_chans_dirty, 1);   /* persiste (débounce) */
		(void)!write(fd, out, strlen(out));
		return;
	}

	if (!strncmp(req, "status", 6)) {
		int n = snprintf(out, sizeof(out),
				 "{\"ok\":true,\"sf2\":\"%s\",\"gain\":%.3f,"
				 "\"chans\":[", g_sf2_name,
				 fluid_synth_get_gain(g_synth));
		for (int c = 0; c < 16; c++) {
			int sf, bank, prog = 0;
			fluid_synth_get_program(g_synth, c, &sf, &bank, &prog);
			n += snprintf(out + n, sizeof(out) - n, "%s%d",
				      c ? "," : "", prog);
		}
		n += snprintf(out + n, sizeof(out) - n, "],\"act\":[");
		for (int c = 0; c < 16; c++)
			n += snprintf(out + n, sizeof(out) - n, "%s%u",
				      c ? "," : "",
				      atomic_load_explicit(&g_act[c],
							   memory_order_relaxed));
		n += snprintf(out + n, sizeof(out) - n, "]");
		n += sy_status_json(out + n, sizeof(out) - n);   /* V12-SYNTH */
		snprintf(out + n, sizeof(out) - n, "}\n");
	} else if (sscanf(req, "prog %d %d", &chan, &num) == 2 &&
		   chan >= 0 && chan < 16 && num >= 0 && num < 128) {
		fluid_synth_program_change(g_synth, chan, num);
		atomic_store(&g_chans_dirty, 1);
		snprintf(out, sizeof(out),
			 "{\"ok\":true,\"chan\":%d,\"prog\":%d}\n", chan, num);
	} else if (sscanf(req, "gain %f", &val) == 1 &&
		   val >= 0.0f && val <= 10.0f) {
		fluid_synth_set_gain(g_synth, val);
		atomic_store(&g_chans_dirty, 1);
		snprintf(out, sizeof(out), "{\"ok\":true,\"gain\":%.3f}\n", val);
	} else if (!strncmp(req, "panic", 5)) {
		for (int c = 0; c < 16; c++) {
			fluid_synth_all_notes_off(g_synth, c);
			fluid_synth_all_sounds_off(g_synth, c);
		}
		snprintf(out, sizeof(out), "{\"ok\":true,\"op\":\"panic\"}\n");
	} else {
		snprintf(out, sizeof(out), "{\"ok\":false,\"err\":\"bad cmd\"}\n");
	}
	(void)!write(fd, out, strlen(out));
}

static void *ctl_thread(void *arg)
{
	(void)arg;
	unlink(CTL_SOCK);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ls < 0) return NULL;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", CTL_SOCK);
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(ls, 4) < 0) {
		mlog("midix: ctl socket: %m");
		close(ls);
		return NULL;
	}
	time_t last_save = 0;
	while (g_run) {
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(ls, &rf);
		int r = select(ls + 1, &rf, NULL, NULL, &tv);
		/* persistance débouncée 2 s, hors requête */
		if (atomic_load(&g_chans_dirty) &&
		    time(NULL) - last_save >= 2) {
			atomic_store(&g_chans_dirty, 0);
			chans_save();
			last_save = time(NULL);
		}
		if (r <= 0)
			continue;
		int fd = accept(ls, NULL, NULL);
		if (fd < 0)
			continue;
		struct timeval rt = { .tv_sec = 0, .tv_usec = 300000 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
		char req[128];
		ssize_t n = read(fd, req, sizeof(req) - 1);
		if (n > 0) {
			req[n] = '\0';
			ctl_handle(fd, req);
		}
		close(fd);
	}
	close(ls);
	unlink(CTL_SOCK);
	if (atomic_load(&g_chans_dirty))
		chans_save();
	return NULL;
}

/* Trouve la carte ALSA du gadget f_midi → "hw:N" (id "fmidi"/"f_midi") */
static int find_fmidi_card(char *dev, size_t n)
{
	FILE *f = fopen("/proc/asound/cards", "r");
	if (!f) return -1;
	char line[256];
	int card = -1;
	while (fgets(line, sizeof(line), f)) {
		int c;
		char id[64];
		if (sscanf(line, " %d [%63s", &c, id) == 2) {
			/* id se termine par ']' ou espaces */
			char *b = strchr(id, ']');
			if (b) *b = '\0';
			if (strcasestr(id, "midi")) { card = c; break; }
		}
	}
	fclose(f);
	if (card < 0) return -1;
	snprintf(dev, n, "hw:%d,0", card);
	return 0;
}

int main(void)
{
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	/* --- config --- */
	char sf2[512] = SF2_DEFAULT;
	double gain = 0.5;
	int polyphony = 64;
	FILE *cf = fopen(CONF_PATH, "r");
	if (cf) {
		char line[600];
		while (fgets(line, sizeof(line), cf)) {
			char v[512];
			if (sscanf(line, "sf2_path=%511s", v) == 1)
				snprintf(sf2, sizeof(sf2), "%s", v);
			else if (sscanf(line, "gain=%lf", &gain) == 1) {}
			else if (sscanf(line, "polyphony=%d", &polyphony) == 1) {}
		}
		fclose(cf);
	}

	/* --- ring SHM (producer = owner) --- */
	size_t shm_sz = sizeof(struct midix_hdr)
	                + (size_t)RING_FRAMES * 2 * sizeof(float);
	int fd = shm_open(MIDIX_SHM, O_CREAT | O_RDWR, 0644);
	if (fd < 0) { mlog("midix: shm_open: %m"); return 1; }
	if (ftruncate(fd, (off_t)shm_sz) < 0) { mlog("midix: ftruncate: %m"); return 1; }
	void *m = mmap(NULL, shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) { mlog("midix: mmap: %m"); return 1; }
	struct midix_hdr *hdr = m;
	float *ring = (float *)((char *)m + sizeof(*hdr));
	memset(ring, 0, (size_t)RING_FRAMES * 2 * sizeof(float));
	hdr->ring_frames = RING_FRAMES;
	atomic_store(&hdr->widx, 0);
	hdr->magic = MIDIX_MAGIC;     /* magic EN DERNIER (validité) */
	mlockall(MCL_CURRENT | MCL_FUTURE);

	/* --- fluidsynth --- */
	fluid_settings_t *st = new_fluid_settings();
	fluid_settings_setnum(st, "synth.sample-rate", SAMPLE_RATE);
	fluid_settings_setint(st, "synth.polyphony", polyphony);
	fluid_settings_setnum(st, "synth.gain", gain);
	fluid_settings_setint(st, "synth.reverb.active", 0);
	fluid_settings_setint(st, "synth.chorus.active", 0);
	fluid_settings_setint(st, "synth.cpu-cores", 1);
	fluid_synth_t *synth = new_fluid_synth(st);
	if (!synth) { mlog("midix: new_fluid_synth FAILED"); return 1; }
	if (fluid_synth_sfload(synth, sf2, 1) < 0) {
		mlog("midix: sfload '%s' FAILED", sf2);
		return 1;
	}
	mlog("midix: SoundFont '%s' chargé (gain %.2f, poly %d)",
	     sf2, gain, polyphony);

	/* V12-MIDIX-GUI : contrôle GUI (programmes par canal) + persistance */
	g_synth = synth;
	const char *bn = strrchr(sf2, '/');
	snprintf(g_sf2_name, sizeof(g_sf2_name), "%s", bn ? bn + 1 : sf2);
	mkdir("/var/lib/ala", 0755);

	/* V12-SYNTH : moteur M1 (multisamples de la même SF2). Échec =
	 * dégradation propre, GM seul. AVANT chans_load (engine lines). */
	if (sy_init(sf2) == 0)
		mlog("midix: moteur M1 prêt (multisamples SF2)");
	else
		mlog("midix: moteur M1 indisponible (parse SF2) — GM seul");
	chans_load();
	pthread_t th_ctl;
	pthread_create(&th_ctl, NULL, ctl_thread, NULL);

	/* --- MIDI in : driver alsa_raw sur la carte f_midi (retry si absent,
	 * le gadget peut arriver après nous malgré After=) --- */
	fluid_midi_driver_t *mdrv = NULL;
	char dev[32] = "";
	for (int tries = 0; tries < 30 && g_run; tries++) {
		if (find_fmidi_card(dev, sizeof(dev)) == 0) break;
		sleep(1);
	}
	if (dev[0]) {
		fluid_settings_setstr(st, "midi.alsa.device", dev);
		fluid_settings_setstr(st, "midi.driver", "alsa_raw");
		/* V12-VU : callback wrapper — alimente g_act[] puis relaie */
		mdrv = new_fluid_midi_driver(st, midi_ev_cb, synth);
		mlog("midix: MIDI in sur %s%s", dev,
		     mdrv ? "" : " (driver FAILED)");
	} else {
		mlog("midix: carte f_midi introuvable — rendu muet (retry service)");
	}

	/* --- boucle de rendu : 96 frames toutes les 2 ms, horloge absolue --- */
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	float buf[PERIOD_FRAMES * 2];
	while (g_run) {
		next.tv_nsec += 2000000L;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		if (fluid_synth_write_float(synth, PERIOD_FRAMES,
		                            buf, 0, 2, buf, 1, 2) != FLUID_OK)
			memset(buf, 0, sizeof(buf));

		/* V12-SYNTH : moteur M1 additionné (canaux assignés M1) */
		sy_render_add(buf, PERIOD_FRAMES);

		/* V12-VU : retombée des vumètres d'activité (~500 ms) */
		for (int c = 0; c < 16; c++) {
			uint32_t a = atomic_load_explicit(&g_act[c],
							  memory_order_relaxed);
			if (a)
				atomic_store_explicit(&g_act[c],
						      (a * 995) / 1000,
						      memory_order_relaxed);
		}

		uint32_t w = atomic_load_explicit(&hdr->widx,
						  memory_order_relaxed);
		uint32_t pos = w % RING_FRAMES;
		/* pos toujours multiple de 96 (widx avance par 96) → un memcpy */
		memcpy(&ring[(size_t)pos * 2], buf, sizeof(buf));
		atomic_store_explicit(&hdr->widx, w + PERIOD_FRAMES,
				      memory_order_release);
	}

	pthread_join(th_ctl, NULL);   /* le thread ctl sauve l'état avant exit */
	if (mdrv) delete_fluid_midi_driver(mdrv);
	delete_fluid_synth(synth);
	delete_fluid_settings(st);
	hdr->magic = 0;
	munmap(m, shm_sz);
	shm_unlink(MIDIX_SHM);
	mlog("midix: arrêt propre");
	return 0;
}
