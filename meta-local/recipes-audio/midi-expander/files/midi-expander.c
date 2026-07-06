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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fluidsynth.h>

#define SAMPLE_RATE   48000
#define PERIOD_FRAMES 96
#define RING_PERIODS  256                      /* ≈ 0,5 s de marge */
#define RING_FRAMES   (RING_PERIODS * PERIOD_FRAMES)
#define MIDIX_SHM     "/ala-midix"
#define MIDIX_MAGIC   0x4D494458u              /* 'MIDX' */
#define CONF_PATH     "/etc/ala/midi-expander.conf"
#define SF2_DEFAULT   "/usr/share/sounds/sf2/GeneralUserGS.sf2"

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
		mdrv = new_fluid_midi_driver(st, fluid_synth_handle_midi_event,
		                             synth);
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

		uint32_t w = atomic_load_explicit(&hdr->widx,
						  memory_order_relaxed);
		uint32_t pos = w % RING_FRAMES;
		/* pos toujours multiple de 96 (widx avance par 96) → un memcpy */
		memcpy(&ring[(size_t)pos * 2], buf, sizeof(buf));
		atomic_store_explicit(&hdr->widx, w + PERIOD_FRAMES,
				      memory_order_release);
	}

	if (mdrv) delete_fluid_midi_driver(mdrv);
	delete_fluid_synth(synth);
	delete_fluid_settings(st);
	hdr->magic = 0;
	munmap(m, shm_sz);
	shm_unlink(MIDIX_SHM);
	mlog("midix: arrêt propre");
	return 0;
}
