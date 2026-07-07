/* SPDX-License-Identifier: MIT
 *
 * V12-SYNTH — moteur « M1-like » (AI Synthesis) de l'expandeur A.L.A.
 *
 * Architecture (esprit Korg M1, 1988) :
 *   OSC1 + OSC2 : lecture de multisamples PCM (zones keyRange de la SF2
 *                 embarquée, parseur RIFF minimal en lecture seule),
 *                 interpolation linéaire, boucle, detune ±50 cents.
 *   VDF : passe-bas 2 × 1 pôle SANS résonance (authentique M1),
 *         cutoff 0-99 (100 Hz → 12 kHz log) + EG_int × EG_vdf.
 *   VDA : gain × EG_vda × vélocité^sens.
 *   EG  : forme ADBSSR — Attack t, Decay t → Break level, Slope t →
 *         Sustain level, Release t. Paramètres 0-99 comme le M1.
 *   LFO : triangle → pitch (vibrato), delay de fade-in.
 *   16 voix, vol de la plus ancienne.
 *
 * Threading : le thread MIDI pousse dans une file SPSC ; le thread de
 * rendu (2 ms) draine et rend ; le thread ctl édite les patches (ints,
 * races bénignes — les voix copient le patch à la note-on, cutoff/level
 * sont relus par bloc pour l'édition live). ARCHI_V12_SYNTH_M1.md. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "ala-synth.h"

#define SY_VOICES   16
#define SY_PATCHES  16
#define SY_MAXINST  512
#define SY_MAXZONES 4096
#define SY_RATE     48000.0f
#define PATCH_CONF  "/var/lib/ala/synth-patches.conf"

/* ============================ SF2 (lecture seule) ============================ */

struct sf_zone {
	uint8_t  klo, khi;      /* keyRange */
	uint8_t  root;          /* rootkey effectif */
	uint8_t  loop;          /* sampleModes & 1 */
	uint32_t start, end;    /* frames absolus dans smpl */
	uint32_t lstart, lend;  /* boucle */
	uint32_t srate;
};
struct sf_inst {
	char name[21];
	int  z0, nz;            /* index + nombre de zones */
};

static const int16_t  *g_smpl;       /* PCM s16 mmappé */
static uint32_t        g_smpl_n;
static struct sf_inst  g_inst[SY_MAXINST];
static int             g_ninst;
static struct sf_zone  g_zone[SY_MAXZONES];
static int             g_nzone;
static void           *g_sf_map;
static size_t          g_sf_sz;

/* structures brutes SF2 (packées) */
#pragma pack(push, 1)
struct sf_shdr {
	char name[20];
	uint32_t start, end, lstart, lend, srate;
	uint8_t origpitch; int8_t corr; uint16_t link, type;
};
struct sf_instrec { char name[20]; uint16_t bagndx; };
struct sf_bag { uint16_t genndx, modndx; };
struct sf_gen { uint16_t oper; uint16_t amount; };
#pragma pack(pop)

static const uint8_t *find_chunk(const uint8_t *p, const uint8_t *end,
				 const char *id, uint32_t *sz)
{
	while (p + 8 <= end) {
		uint32_t csz;
		memcpy(&csz, p + 4, 4);
		if (!memcmp(p, id, 4)) { *sz = csz; return p + 8; }
		p += 8 + csz + (csz & 1);
	}
	return NULL;
}

static int sf2_parse(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	struct stat st;
	if (fstat(fd, &st) < 0) { close(fd); return -1; }
	g_sf_sz = (size_t)st.st_size;
	g_sf_map = mmap(NULL, g_sf_sz, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (g_sf_map == MAP_FAILED) return -1;
	const uint8_t *base = g_sf_map, *end = base + g_sf_sz;
	if (g_sf_sz < 12 || memcmp(base, "RIFF", 4) || memcmp(base + 8, "sfbk", 4))
		return -1;

	const uint8_t *smpl = NULL;
	const struct sf_shdr *shdr = NULL;
	const struct sf_instrec *inst = NULL;
	const struct sf_bag *ibag = NULL;
	const struct sf_gen *igen = NULL;
	uint32_t n_shdr = 0, n_inst = 0, n_ibag = 0, n_igen = 0, smpl_sz = 0;

	/* itère les LIST du niveau RIFF */
	const uint8_t *p = base + 12;
	while (p + 8 <= end) {
		uint32_t csz;
		memcpy(&csz, p + 4, 4);
		if (!memcmp(p, "LIST", 4) && p + 12 <= end) {
			const uint8_t *lp = p + 12, *lend = p + 8 + csz;
			uint32_t s;
			if (!memcmp(p + 8, "sdta", 4)) {
				const uint8_t *c = find_chunk(lp, lend, "smpl", &s);
				if (c) { smpl = c; smpl_sz = s; }
			} else if (!memcmp(p + 8, "pdta", 4)) {
				const uint8_t *c;
				if ((c = find_chunk(lp, lend, "inst", &s)))
					{ inst = (const void *)c; n_inst = s / 22; }
				if ((c = find_chunk(lp, lend, "ibag", &s)))
					{ ibag = (const void *)c; n_ibag = s / 4; }
				if ((c = find_chunk(lp, lend, "igen", &s)))
					{ igen = (const void *)c; n_igen = s / 4; }
				if ((c = find_chunk(lp, lend, "shdr", &s)))
					{ shdr = (const void *)c; n_shdr = s / 46; }
			}
		}
		p += 8 + csz + (csz & 1);
	}
	if (!smpl || !shdr || !inst || !ibag || !igen || n_inst < 2)
		return -1;
	g_smpl = (const int16_t *)smpl;
	g_smpl_n = smpl_sz / 2;

	/* construit instruments + zones (générateurs essentiels seulement) */
	for (uint32_t i = 0; i + 1 < n_inst && g_ninst < SY_MAXINST; i++) {
		struct sf_inst *out = &g_inst[g_ninst];
		memcpy(out->name, inst[i].name, 20);
		out->name[20] = '\0';
		out->z0 = g_nzone;
		out->nz = 0;
		uint16_t b0 = inst[i].bagndx, b1 = inst[i + 1].bagndx;
		/* zone GLOBALE (1er bag sans sampleID) = défauts hérités par
		 * les zones de l'instrument — GeneralUser y met sampleModes */
		int gmode = 0, groot = -1;
		for (uint16_t b = b0; b < b1 && b + 1 < n_ibag; b++) {
			uint16_t gi0 = ibag[b].genndx, gi1 = ibag[b + 1].genndx;
			int klo = 0, khi = 127, root = groot, mode = gmode,
			    sid = -1;
			for (uint16_t g = gi0; g < gi1 && g < n_igen; g++) {
				uint16_t op = igen[g].oper, am = igen[g].amount;
				if (op == 43) { klo = am & 0xff; khi = am >> 8; }
				else if (op == 58) root = (int16_t)am;
				else if (op == 54) mode = am;
				else if (op == 53) sid = am;
			}
			if (sid < 0) {   /* zone globale : mémorise les défauts */
				if (b == b0) { gmode = mode; groot = root; }
				continue;
			}
			if ((uint32_t)sid >= n_shdr || g_nzone >= SY_MAXZONES)
				continue;
			const struct sf_shdr *sh = &shdr[sid];
			if (sh->end <= sh->start || sh->end > g_smpl_n)
				continue;
			struct sf_zone *z = &g_zone[g_nzone];
			z->klo = (uint8_t)klo; z->khi = (uint8_t)khi;
			z->root = (uint8_t)(root >= 0 ? root : sh->origpitch);
			z->loop = (mode & 1) &&
				  sh->lend > sh->lstart && sh->lend <= sh->end;
			z->start = sh->start; z->end = sh->end;
			z->lstart = sh->lstart; z->lend = sh->lend;
			z->srate = sh->srate ? sh->srate : 44100;
			g_nzone++;
			out->nz++;
		}
		if (out->nz > 0)
			g_ninst++;
	}
	return g_ninst > 0 ? 0 : -1;
}

static int zone_for(int inst, int note)
{
	if (inst < 0 || inst >= g_ninst) return -1;
	const struct sf_inst *in = &g_inst[inst];
	int best = -1, bestd = 999;
	for (int i = 0; i < in->nz; i++) {
		const struct sf_zone *z = &g_zone[in->z0 + i];
		if (note >= z->klo && note <= z->khi)
			return in->z0 + i;
		int d = note < z->klo ? z->klo - note : note - z->khi;
		if (d < bestd) { bestd = d; best = in->z0 + i; }
	}
	return best;   /* zone la plus proche si hors range */
}

/* ============================ Patches ============================ */

struct sy_patch {
	char name[16];
	int  osc1, osc2;          /* instrument, osc2 -1 = single */
	int  detune;              /* cents -50..50 */
	int  balance;             /* 0-99 (0 = osc1 seul) */
	int  cutoff, eg_int;      /* VDF 0-99 */
	int  vdf[6], vda[6];      /* ADBSSR : a,d,break_l,sustain_l,slope_t,r 0-99 */
	int  lfo_rate, lfo_depth, lfo_delay;   /* 0-99 */
	int  vel_sens;            /* 0-99 */
	int  level;               /* 0-99 */
};
static struct sy_patch g_patch[SY_PATCHES];
static _Atomic int g_patch_dirty;

/* assignation par canal : 0 = GM (fluidsynth), 1 = M1 */
static _Atomic int g_ceng[16];
static _Atomic int g_cpatch[16];

static int find_inst(const char *needle)
{
	for (int i = 0; i < g_ninst; i++)
		if (strcasestr(g_inst[i].name, needle))
			return i;
	return 0;
}

static void patch_defaults(void)
{
	/* needles = noms réels de la banque GeneralUser GS */
	static const struct { const char *name, *inst;
		int cutoff, egint, vdfr, vdaa, vdar, lfod; } F[4] = {
		{ "M1 Piano",  "Stereo Grand",  85, 20, 40,  1, 35, 0 },
		{ "Warm Pad",  "Synth Strings", 55, 35, 70, 55, 75, 8 },
		{ "Deep Bass", "Acoustic Bass", 45, 45, 30,  2, 25, 0 },
		{ "Bells",     "Celeste",       90, 10, 60,  1, 70, 4 },
	};
	for (int i = 0; i < SY_PATCHES; i++) {
		struct sy_patch *P = &g_patch[i];
		const int f = i < 4 ? i : 0;
		snprintf(P->name, sizeof(P->name), "%s",
			 i < 4 ? F[f].name : "Init");
		P->osc1 = find_inst(F[f].inst);
		P->osc2 = -1;
		P->detune = 6; P->balance = 50;
		P->cutoff = F[f].cutoff; P->eg_int = F[f].egint;
		P->vdf[0] = 2;  P->vdf[1] = 45; P->vdf[2] = 60;
		P->vdf[3] = 45; P->vdf[4] = 40; P->vdf[5] = F[f].vdfr;
		P->vda[0] = F[f].vdaa; P->vda[1] = 50; P->vda[2] = 85;
		P->vda[3] = 75; P->vda[4] = 50; P->vda[5] = F[f].vdar;
		P->lfo_rate = 45; P->lfo_depth = F[f].lfod; P->lfo_delay = 40;
		P->vel_sens = 60; P->level = 80;
	}
}

static void patch_save_all(void)
{
	FILE *f = fopen(PATCH_CONF ".tmp", "w");
	if (!f) return;
	for (int i = 0; i < SY_PATCHES; i++) {
		struct sy_patch *P = &g_patch[i];
		fprintf(f, "patch %d %d %d %d %d %d %d "
			   "%d %d %d %d %d %d  %d %d %d %d %d %d "
			   "%d %d %d %d %d %s\n",
			i, P->osc1, P->osc2, P->detune, P->balance,
			P->cutoff, P->eg_int,
			P->vdf[0], P->vdf[1], P->vdf[2], P->vdf[3], P->vdf[4], P->vdf[5],
			P->vda[0], P->vda[1], P->vda[2], P->vda[3], P->vda[4], P->vda[5],
			P->lfo_rate, P->lfo_depth, P->lfo_delay,
			P->vel_sens, P->level, P->name);
	}
	fclose(f);
	rename(PATCH_CONF ".tmp", PATCH_CONF);
}

static void patch_load_all(void)
{
	FILE *f = fopen(PATCH_CONF, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		struct sy_patch T;
		int i, n;
		if (sscanf(line, "patch %d %d %d %d %d %d %d "
				 "%d %d %d %d %d %d %d %d %d %d %d %d "
				 "%d %d %d %d %d %n",
			   &i, &T.osc1, &T.osc2, &T.detune, &T.balance,
			   &T.cutoff, &T.eg_int,
			   &T.vdf[0], &T.vdf[1], &T.vdf[2], &T.vdf[3], &T.vdf[4], &T.vdf[5],
			   &T.vda[0], &T.vda[1], &T.vda[2], &T.vda[3], &T.vda[4], &T.vda[5],
			   &T.lfo_rate, &T.lfo_depth, &T.lfo_delay,
			   &T.vel_sens, &T.level, &n) == 24 &&
		    i >= 0 && i < SY_PATCHES) {
			char *nl = strchr(line + n, '\n');
			if (nl) *nl = '\0';
			snprintf(T.name, sizeof(T.name), "%s",
				 line[n] ? line + n : "Init");
			if (T.osc1 < 0 || T.osc1 >= g_ninst) T.osc1 = 0;
			if (T.osc2 >= g_ninst) T.osc2 = -1;
			g_patch[i] = T;
		}
	}
	fclose(f);
}

/* ============================ Voix ============================ */

/* mappings 0-99 (esprit M1) */
static float p2time(int p)     /* 1 ms → 10 s log */
{ return 0.001f * expf((float)p / 99.0f * 9.21034f); }
static float p2cut(int p)      /* 100 Hz → 12 kHz log */
{ return 100.0f * expf((float)p / 99.0f * 4.7875f); }

struct sy_eg {
	int   seg;        /* 0 att, 1 dec, 2 slope, 3 sustain, 4 rel, 5 off */
	float val, from;
	float t, dur;     /* temps dans le segment */
	float bl, sl;     /* break/sustain levels 0-1 */
	float ta, td, ts, tr;
};

static void eg_start(struct sy_eg *e, const int p[6])
{
	e->ta = p2time(p[0]); e->td = p2time(p[1]);
	e->bl = p[2] / 99.0f; e->sl = p[3] / 99.0f;
	e->ts = p2time(p[4]); e->tr = p2time(p[5]);
	e->seg = 0; e->val = 0; e->from = 0;
	e->t = 0; e->dur = e->ta > 0.0015f ? e->ta : 0.0015f;
}
static void eg_release(struct sy_eg *e)
{
	if (e->seg >= 4) return;
	e->seg = 4; e->from = e->val; e->t = 0;
	e->dur = e->tr > 0.003f ? e->tr : 0.003f;
}
/* avance d'un bloc (dt s), retourne la valeur cible fin de bloc */
static float eg_step(struct sy_eg *e, float dt)
{
	if (e->seg == 5) return 0;
	if (e->seg == 3) { e->val = e->sl; return e->val; }   /* sustain */
	e->t += dt;
	float x = e->t / e->dur;
	if (x > 1.0f) x = 1.0f;
	float tgt = 0;
	switch (e->seg) {
	case 0: tgt = 1.0f; break;
	case 1: tgt = e->bl; break;
	case 2: tgt = e->sl; break;
	case 4: tgt = 0.0f; break;
	}
	e->val = e->from + (tgt - e->from) * x;
	if (x >= 1.0f) {
		e->from = e->val; e->t = 0;
		switch (e->seg) {
		case 0: e->seg = 1; e->dur = e->td > 0.002f ? e->td : 0.002f; break;
		case 1: e->seg = 2; e->dur = e->ts > 0.002f ? e->ts : 0.002f; break;
		case 2: e->seg = 3; break;
		case 4: e->seg = 5; e->val = 0; break;
		}
	}
	return e->val;
}

struct sy_osc {
	int    zone;      /* -1 = off */
	double ph, inc;   /* position/incrément (frames sample) */
	int    done;
};
struct sy_voice {
	int    active, chan, note;
	float  vgain;               /* vélocité^sens × level */
	int    pidx;                /* patch (relu par bloc : cutoff/level live) */
	float  bal;                 /* balance copiée */
	struct sy_osc o1, o2;
	struct sy_eg  egf, ega;
	float  lfo_ph, lfo_age;
	float  f1, f2;              /* états filtre */
	float  lastg;               /* VDA lissé intra-bloc */
	uint32_t age;
};
static struct sy_voice g_v[SY_VOICES];
static uint32_t g_vage;

/* file SPSC d'événements MIDI (thread MIDI → rendu) */
struct sy_ev { uint8_t type, chan, d1, d2; };
static struct sy_ev g_evq[256];
static _Atomic uint32_t g_evw, g_evr;

void sy_midi(int type, int chan, int d1, int d2)
{
	uint32_t w = atomic_load_explicit(&g_evw, memory_order_relaxed);
	if (w - atomic_load_explicit(&g_evr, memory_order_acquire) >= 256)
		return;   /* file pleine : on jette (jamais de blocage) */
	g_evq[w % 256] = (struct sy_ev){ (uint8_t)type, (uint8_t)chan,
					 (uint8_t)d1, (uint8_t)d2 };
	atomic_store_explicit(&g_evw, w + 1, memory_order_release);
}

int sy_chan_is_m1(int chan)
{
	return chan >= 0 && chan < 16 &&
	       atomic_load_explicit(&g_ceng[chan], memory_order_relaxed) == 1;
}

static void osc_setup(struct sy_osc *o, int inst, int note, float cents)
{
	o->zone = zone_for(inst, note);
	o->done = o->zone < 0;
	if (o->done) return;
	const struct sf_zone *z = &g_zone[o->zone];
	o->ph = (double)z->start;
	float semis = (float)(note - z->root) + cents / 100.0f;
	o->inc = pow(2.0, semis / 12.0) * ((double)z->srate / SY_RATE);
}

static void note_on(int chan, int note, int vel)
{
	int pi = atomic_load_explicit(&g_cpatch[chan], memory_order_relaxed);
	if (pi < 0 || pi >= SY_PATCHES) pi = 0;
	const struct sy_patch *P = &g_patch[pi];

	/* alloc : libre, sinon la plus ancienne */
	struct sy_voice *v = NULL;
	uint32_t oldest = 0xFFFFFFFFu;
	for (int i = 0; i < SY_VOICES; i++) {
		if (!g_v[i].active) { v = &g_v[i]; break; }
		if (g_v[i].age < oldest) { oldest = g_v[i].age; v = &g_v[i]; }
	}
	memset(v, 0, sizeof(*v));
	v->active = 1; v->chan = chan; v->note = note;
	v->pidx = pi; v->age = ++g_vage;
	v->bal = P->osc2 >= 0 ? P->balance / 99.0f : 0.0f;
	float sens = P->vel_sens / 99.0f;
	float nv = vel / 127.0f;
	v->vgain = (1.0f - sens) + sens * nv * nv;   /* réponse quadratique */
	osc_setup(&v->o1, P->osc1, note, 0.0f);
	if (P->osc2 >= 0)
		osc_setup(&v->o2, P->osc2, note, (float)P->detune);
	else
		v->o2.done = 1;
	eg_start(&v->egf, P->vdf);
	eg_start(&v->ega, P->vda);
	v->lastg = 0.0f;
}

static void note_off(int chan, int note)
{
	for (int i = 0; i < SY_VOICES; i++)
		if (g_v[i].active && g_v[i].chan == chan && g_v[i].note == note)
			{ eg_release(&g_v[i].egf); eg_release(&g_v[i].ega); }
}

static inline float osc_fetch(struct sy_osc *o)
{
	if (o->done) return 0.0f;
	const struct sf_zone *z = &g_zone[o->zone];
	uint32_t i = (uint32_t)o->ph;
	if (i + 1 >= z->end) {
		if (z->loop) {
			o->ph -= (double)(z->lend - z->lstart);
			i = (uint32_t)o->ph;
			if (i + 1 >= z->end) { o->done = 1; return 0.0f; }
		} else { o->done = 1; return 0.0f; }
	}
	float frac = (float)(o->ph - i);
	float a = g_smpl[i] * (1.0f / 32768.0f);
	float b = g_smpl[i + 1] * (1.0f / 32768.0f);
	o->ph += o->inc;
	if (z->loop && o->ph >= (double)z->lend)
		o->ph -= (double)(z->lend - z->lstart);
	return a + (b - a) * frac;
}

void sy_render_add(float *buf, int frames)
{
	/* draine la file d'événements */
	uint32_t r = atomic_load_explicit(&g_evr, memory_order_relaxed);
	uint32_t w = atomic_load_explicit(&g_evw, memory_order_acquire);
	while (r != w) {
		struct sy_ev *e = &g_evq[r % 256];
		if (e->type == 0x90 && e->d2 > 0)      note_on(e->chan, e->d1, e->d2);
		else if (e->type == 0x80 ||
			 (e->type == 0x90 && e->d2 == 0)) note_off(e->chan, e->d1);
		else if (e->type == 0xB0 &&
			 (e->d1 == 120 || e->d1 == 123)) {   /* all off */
			for (int i = 0; i < SY_VOICES; i++)
				if (g_v[i].active && g_v[i].chan == e->chan)
					{ eg_release(&g_v[i].egf); eg_release(&g_v[i].ega); }
		}
		r++;
	}
	atomic_store_explicit(&g_evr, r, memory_order_release);

	const float dt = (float)frames / SY_RATE;
	for (int i = 0; i < SY_VOICES; i++) {
		struct sy_voice *v = &g_v[i];
		if (!v->active)
			continue;
		const struct sy_patch *P = &g_patch[v->pidx];

		/* enveloppes par bloc + lissage intra-bloc du VDA */
		float egf = eg_step(&v->egf, dt);
		float ega = eg_step(&v->ega, dt);
		if (v->ega.seg == 5 || (v->o1.done && v->o2.done)) {
			v->active = 0;
			continue;
		}
		/* LFO pitch (recalc inc par bloc) */
		v->lfo_age += dt;
		float lfo = 0.0f;
		if (P->lfo_depth > 0) {
			float rate = 0.1f + P->lfo_rate / 99.0f * 7.9f;
			v->lfo_ph += rate * dt;
			if (v->lfo_ph >= 1.0f) v->lfo_ph -= 1.0f;
			float tri = v->lfo_ph < 0.5f
				    ? v->lfo_ph * 4.0f - 1.0f
				    : 3.0f - v->lfo_ph * 4.0f;
			float dl = p2time(P->lfo_delay);
			float fade = dl > 0 ? (v->lfo_age > dl ? 1.0f
					       : v->lfo_age / dl) : 1.0f;
			lfo = tri * (P->lfo_depth / 99.0f) * 0.5f * fade; /* ±½ ton max */
		}
		float pmod = lfo != 0.0f ? powf(2.0f, lfo / 12.0f) : 1.0f;

		/* VDF : cutoff (live) + EG */
		float cut = p2cut(P->cutoff) *
			    powf(2.0f, (P->eg_int / 99.0f) * 4.0f * egf);
		if (cut > 18000.0f) cut = 18000.0f;
		float k = 1.0f - expf(-6.2831853f * cut / SY_RATE);

		float lvl = (P->level / 99.0f) * v->vgain;
		float g0 = v->lastg, g1 = ega * lvl;
		float gstep = (g1 - g0) / (float)frames;
		v->lastg = g1;

		float bal = v->bal;
		double inc1 = v->o1.inc * pmod, inc2 = v->o2.inc * pmod;
		double sv1 = v->o1.inc, sv2 = v->o2.inc;
		v->o1.inc = inc1; v->o2.inc = inc2;

		float f1 = v->f1, f2 = v->f2, g = g0;
		for (int f = 0; f < frames; f++) {
			float s = osc_fetch(&v->o1) * (1.0f - bal);
			if (!v->o2.done)
				s += osc_fetch(&v->o2) * bal;
			f1 += k * (s - f1);
			f2 += k * (f1 - f2);
			g += gstep;
			float out = f2 * g;
			buf[f * 2]     += out;
			buf[f * 2 + 1] += out;
		}
		v->f1 = f1; v->f2 = f2;
		v->o1.inc = sv1; v->o2.inc = sv2;
	}
}

/* ============================ Contrôle / état ============================ */

int sy_status_json(char *out, size_t outsz)
{
	int n = snprintf(out, outsz, ",\"engines\":[");
	for (int c = 0; c < 16; c++)
		n += snprintf(out + n, outsz - n, "%s%d", c ? "," : "",
			      atomic_load_explicit(&g_ceng[c], memory_order_relaxed));
	n += snprintf(out + n, outsz - n, "],\"patch\":[");
	for (int c = 0; c < 16; c++)
		n += snprintf(out + n, outsz - n, "%s%d", c ? "," : "",
			      atomic_load_explicit(&g_cpatch[c], memory_order_relaxed));
	n += snprintf(out + n, outsz - n, "]");
	return n;
}

void sy_save_chans(FILE *f)
{
	for (int c = 0; c < 16; c++)
		fprintf(f, "engine %d %d %d\n", c,
			atomic_load(&g_ceng[c]), atomic_load(&g_cpatch[c]));
}

int sy_load_chan_line(const char *line)
{
	int c, e, p;
	if (sscanf(line, "engine %d %d %d", &c, &e, &p) == 3 &&
	    c >= 0 && c < 16) {
		atomic_store(&g_ceng[c], e ? 1 : 0);
		if (p >= 0 && p < SY_PATCHES) atomic_store(&g_cpatch[c], p);
		return 1;
	}
	return 0;
}

static int patch_set_param(struct sy_patch *P, const char *key, int val)
{
	if (!strcmp(key, "osc1") && val >= 0 && val < g_ninst) P->osc1 = val;
	else if (!strcmp(key, "osc2") && val >= -1 && val < g_ninst) P->osc2 = val;
	else if (!strcmp(key, "detune") && val >= -50 && val <= 50) P->detune = val;
	else if (!strcmp(key, "balance")) P->balance = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "cutoff")) P->cutoff = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "eg_int")) P->eg_int = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strncmp(key, "vdf", 3) && key[3] >= '0' && key[3] <= '5')
		P->vdf[key[3] - '0'] = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strncmp(key, "vda", 3) && key[3] >= '0' && key[3] <= '5')
		P->vda[key[3] - '0'] = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "lfo_rate")) P->lfo_rate = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "lfo_depth")) P->lfo_depth = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "lfo_delay")) P->lfo_delay = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "vel_sens")) P->vel_sens = val < 0 ? 0 : val > 99 ? 99 : val;
	else if (!strcmp(key, "level")) P->level = val < 0 ? 0 : val > 99 ? 99 : val;
	else return -1;
	return 0;
}

int sy_ctl(const char *req, char *out, size_t outsz)
{
	int a, b, c;
	char key[24], name[64];

	if (sscanf(req, "engine %d %d %d", &a, &b, &c) >= 2 &&
	    a >= 0 && a < 16) {
		atomic_store(&g_ceng[a], b ? 1 : 0);
		if (sscanf(req, "engine %d %d %d", &a, &b, &c) == 3 &&
		    c >= 0 && c < SY_PATCHES)
			atomic_store(&g_cpatch[a], c);
		atomic_store(&g_patch_dirty, 1);
		snprintf(out, outsz, "{\"ok\":true,\"chan\":%d,\"engine\":%d}\n", a, b);
		return 1;
	}
	if (!strncmp(req, "inst_list", 9)) {
		int n = snprintf(out, outsz, "{\"ok\":true,\"inst\":[");
		for (int i = 0; i < g_ninst && n < (int)outsz - 40; i++)
			n += snprintf(out + n, outsz - n, "%s\"%s\"",
				      i ? "," : "", g_inst[i].name);
		snprintf(out + n, outsz - n, "]}\n");
		return 1;
	}
	if (!strncmp(req, "patch_list", 10)) {
		int n = snprintf(out, outsz, "{\"ok\":true,\"patches\":[");
		for (int i = 0; i < SY_PATCHES; i++)
			n += snprintf(out + n, outsz - n, "%s\"%s\"",
				      i ? "," : "", g_patch[i].name);
		snprintf(out + n, outsz - n, "]}\n");
		return 1;
	}
	if (sscanf(req, "patch_get %d", &a) == 1 && a >= 0 && a < SY_PATCHES) {
		struct sy_patch *P = &g_patch[a];
		snprintf(out, outsz,
			"{\"ok\":true,\"idx\":%d,\"name\":\"%s\","
			"\"osc1\":%d,\"osc2\":%d,\"detune\":%d,\"balance\":%d,"
			"\"cutoff\":%d,\"eg_int\":%d,"
			"\"vdf\":[%d,%d,%d,%d,%d,%d],\"vda\":[%d,%d,%d,%d,%d,%d],"
			"\"lfo_rate\":%d,\"lfo_depth\":%d,\"lfo_delay\":%d,"
			"\"vel_sens\":%d,\"level\":%d,"
			"\"osc1_name\":\"%s\",\"osc2_name\":\"%s\"}\n",
			a, P->name, P->osc1, P->osc2, P->detune, P->balance,
			P->cutoff, P->eg_int,
			P->vdf[0], P->vdf[1], P->vdf[2], P->vdf[3], P->vdf[4], P->vdf[5],
			P->vda[0], P->vda[1], P->vda[2], P->vda[3], P->vda[4], P->vda[5],
			P->lfo_rate, P->lfo_depth, P->lfo_delay,
			P->vel_sens, P->level,
			P->osc1 >= 0 && P->osc1 < g_ninst ? g_inst[P->osc1].name : "-",
			P->osc2 >= 0 && P->osc2 < g_ninst ? g_inst[P->osc2].name : "-");
		return 1;
	}
	if (sscanf(req, "patch_set %d %23s %d", &a, key, &b) == 3 &&
	    a >= 0 && a < SY_PATCHES) {
		if (patch_set_param(&g_patch[a], key, b) == 0) {
			atomic_store(&g_patch_dirty, 1);
			snprintf(out, outsz, "{\"ok\":true}\n");
		} else
			snprintf(out, outsz, "{\"ok\":false,\"err\":\"bad param\"}\n");
		return 1;
	}
	if (sscanf(req, "patch_save %d %63[^\n]", &a, name) >= 1 &&
	    a >= 0 && a < SY_PATCHES) {
		if (sscanf(req, "patch_save %d %63[^\n]", &a, name) == 2)
			snprintf(g_patch[a].name, sizeof(g_patch[a].name),
				 "%s", name);
		patch_save_all();
		atomic_store(&g_patch_dirty, 0);
		snprintf(out, outsz, "{\"ok\":true,\"saved\":%d}\n", a);
		return 1;
	}
	return 0;
}

int sy_init(const char *sf2_path)
{
	if (sf2_parse(sf2_path) < 0)
		return -1;
	patch_defaults();
	patch_load_all();
	return 0;
}

void sy_reload_patches(void)
{
	patch_load_all();   /* les voix actives gardent leur copie ; les
			     * prochaines notes prennent la banque rechargée */
}
