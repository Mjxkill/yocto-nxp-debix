/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V7.0-E7 — mixer-gui-http : GUI HTTP pour mixer-pro
 *
 * Daemon HTTP C (libmicrohttpd) qui :
 *   - sert un GUI statique (HTML + Alpine.js + Tailwind via CDN) depuis /var/www/mixer-gui/
 *   - bridge des appels REST vers le socket Unix /run/mixer-pro.sock
 *   - permet polling rapide (20 Hz) sans surcharger mixer-pro grâce à un pool
 *     de N sockets Unix persistants (round-robin sous mutex).
 *
 * Routes :
 *   GET  /                  → /var/www/mixer-gui/index.html
 *   GET  /static/<path>     → /var/www/mixer-gui/static/<path>
 *   GET  /api/state         → proxy {"op":"get_state"}
 *   POST /api/cmd           → forward body JSON vers mixer-pro
 *   GET  /health            → {"ok":true,"version":"v7.0-e7"}
 *
 * Sécurité : LAN-only, CORS *, validation Content-Type + size max 4 KB sur POST.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <microhttpd.h>
#include <alsa/asoundlib.h>

#define GUI_VERSION       "v8.1b-drift-meter"
#define DEFAULT_PORT      8080
#define MIXER_SOCK_PATH   "/run/mixer-pro.sock"
#define WWW_ROOT          "/var/www/mixer-gui"
/* V9.4.3 : 32 KB pour tenir les payloads ALSA BYTES (DRC blob = 4096 bytes
 * = 8192 chars hex + JSON wrapper). */
#define POST_MAX_BYTES    32768
/* V9.2g-step5h : 10s pour absorber le temps d'instantiate LV2 lourd
 * (LSP/calf peuvent prendre 1-5s à instantiate, surtout en cascade
 * après scan lilv 348 plugins). Avant 200ms → connection refused
 * en bench. */
#define SOCK_RECV_TIMEO_MS 10000
#define MHD_THREAD_POOL   16      /* V10-P2h : marge SSE multi-onglets (2 GUIs x N onglets + kiosk) */
#define STREAM_PERIOD_US  33333   /* E7.1 : 30 Hz SSE */

/* libmicrohttpd 1.0.x : MHD_Result enum introduit récemment.
 * Fallback pour anciennes versions où c'était `int`. */
#ifndef MHD_HTTP_NOT_FOUND
#  define MHD_HTTP_NOT_FOUND 404
#endif

static void mlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	fflush(stderr);
	va_end(ap);
}

/* ============================== Mixer-pro socket bridge ============= */
/* Note : mixer-pro control_thread fait `accept → read → handle → close` séquentiel.
 * Un pool de sockets persistants côté GUI ne marche pas (close passif côté mixer-pro
 * → CLOSE_WAIT silencieux → read timeout sur la 2e requête du même socket).
 * On fait donc un connect/close par requête. Overhead acceptable : ~1-2 ms par
 * requête × 2 Hz polling × 1-5 clients = 2-10 ms/s CPU sur control_thread. */

static int mixer_request(const char *req_line, char *out, size_t out_sz)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(out, out_sz,
			 "{\"ok\":false,\"err\":\"socket: %s\"}\n", strerror(errno));
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, MIXER_SOCK_PATH, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		snprintf(out, out_sz,
			 "{\"ok\":false,\"err\":\"mixer-pro unreachable: %s\"}\n",
			 strerror(errno));
		return -1;
	}
	/* V9.2g-step5h : split en tv_sec + tv_usec car tv_usec doit être < 1e6 */
	struct timeval tv = {
		.tv_sec  = SOCK_RECV_TIMEO_MS / 1000,
		.tv_usec = (SOCK_RECV_TIMEO_MS % 1000) * 1000,
	};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	size_t req_len = strlen(req_line);
	ssize_t w = write(fd, req_line, req_len);
	if (w != (ssize_t)req_len) {
		close(fd);
		snprintf(out, out_sz,
			 "{\"ok\":false,\"err\":\"write failed: %s\"}\n", strerror(errno));
		return -1;
	}

	ssize_t total = 0;
	while (total < (ssize_t)out_sz - 1) {
		ssize_t r = read(fd, out + total, out_sz - 1 - total);
		if (r <= 0) break;
		total += r;
		if (out[total - 1] == '\n') break;
	}
	out[total > 0 ? total : 0] = '\0';
	close(fd);

	if (total <= 0) {
		snprintf(out, out_sz, "{\"ok\":false,\"err\":\"mixer-pro timeout\"}\n");
		return -1;
	}
	return (int)total;
}

/* ============================== SSE streaming ===================== */

/* E7.1 : callback chunked appelé en boucle par MHD pour pousser des frames
 * SSE meters au navigateur. usleep 33 ms entre frames = 30 Hz. Bloque le
 * worker MHD pendant le sleep — c'est OK avec MHD_THREAD_POOL=8.
 * Format RFC 8895 : `data: <json>\n\n` (double newline).
 */
static ssize_t sse_stream_callback(void *cls, uint64_t pos, char *buf, size_t max)
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

#define SC_MAX    6
#define SC_RING   8
#define SC_FRAME  32768

struct sclient {
	int used, is_panel, need_full;
	unsigned wr, rd;               /* indices monotones (slot = idx % RING) */
	int drops;
	int lens[SC_RING];
	pthread_mutex_t mu;
	pthread_cond_t  cv;
};
static struct sclient g_sc[SC_MAX];
static char g_sc_ring[SC_MAX][SC_RING][SC_FRAME];   /* 1.5 MB BSS, statique */
static pthread_mutex_t g_sc_alloc_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_sseq = 0;
static unsigned long g_sc_frames_out = 0;

static char g_sysload_json[256] = "";
static pthread_mutex_t g_sysload_mu = PTHREAD_MUTEX_INITIALIZER;

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

static void sc_broadcast(const char *frame, int len)
{
	for (int i = 0; i < SC_MAX; i++)
		if (g_sc[i].used) sc_push(&g_sc[i], frame, len);
	g_sc_frames_out++;
}

/* construit la trame full depuis les caches ; retourne la longueur */
static int sc_build_full(char *out, size_t sz)
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

static void *state_producer(void *arg)
{
	(void)arg;
	char resp[SC_FRAME/4], frame[SC_FRAME];
	int tick = 0;
	for (;;) {
		usleep(200000);              /* 5 Hz de base */
		tick++;
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
static ssize_t sse_state_cb(void *cls, uint64_t pos, char *buf, size_t max)
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

static void sse_state_free(void *cls)
{
	struct sclient *c = cls;
	pthread_mutex_lock(&g_sc_alloc_mu);
	pthread_mutex_lock(&c->mu);
	c->used = 0;
	pthread_mutex_unlock(&c->mu);
	pthread_mutex_unlock(&g_sc_alloc_mu);
}

static struct sclient *sc_alloc(int is_panel)
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

/* ============================== HTTP helpers ====================== */

static void add_cors_headers(struct MHD_Response *r)
{
	MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
	MHD_add_response_header(r, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	MHD_add_response_header(r, "Access-Control-Allow-Headers", "Content-Type");
	MHD_add_response_header(r, "Cache-Control", "no-store");
}

static enum MHD_Result send_text(struct MHD_Connection *conn, int code,
				 const char *ctype, const char *body, size_t len)
{
	struct MHD_Response *r = MHD_create_response_from_buffer(
		len, (void *)body, MHD_RESPMEM_MUST_COPY);
	if (!r) return MHD_NO;
	MHD_add_response_header(r, "Content-Type", ctype);
	add_cors_headers(r);
	enum MHD_Result ret = MHD_queue_response(conn, code, r);
	MHD_destroy_response(r);
	return ret;
}

static enum MHD_Result send_json(struct MHD_Connection *conn, int code, const char *body)
{
	return send_text(conn, code, "application/json", body, strlen(body));
}

static enum MHD_Result send_file(struct MHD_Connection *conn, const char *path,
				 const char *ctype)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return send_json(conn, MHD_HTTP_NOT_FOUND,
				  "{\"ok\":false,\"err\":\"file not found\"}\n");
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0 || sz > 4 * 1024 * 1024) {
		fclose(fp);
		return send_json(conn, 500, "{\"ok\":false,\"err\":\"file too large\"}\n");
	}
	char *buf = malloc(sz);
	if (!buf) { fclose(fp); return MHD_NO; }
	if (fread(buf, 1, sz, fp) != (size_t)sz) {
		fclose(fp); free(buf);
		return MHD_NO;
	}
	fclose(fp);
	struct MHD_Response *r = MHD_create_response_from_buffer(
		sz, buf, MHD_RESPMEM_MUST_FREE);
	if (!r) { free(buf); return MHD_NO; }
	MHD_add_response_header(r, "Content-Type", ctype);
	add_cors_headers(r);
	enum MHD_Result ret = MHD_queue_response(conn, 200, r);
	MHD_destroy_response(r);
	return ret;
}

static const char *guess_mime(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return "application/octet-stream";
	if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(ext, ".css"))  return "text/css; charset=utf-8";
	if (!strcmp(ext, ".js"))   return "application/javascript; charset=utf-8";
	if (!strcmp(ext, ".json")) return "application/json";
	if (!strcmp(ext, ".svg"))  return "image/svg+xml";
	if (!strcmp(ext, ".png"))  return "image/png";
	if (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg")) return "image/jpeg";
	if (!strcmp(ext, ".ico"))  return "image/x-icon";
	if (!strcmp(ext, ".woff2")) return "font/woff2";
	return "application/octet-stream";
}

/* Validation simple : pas de '..', pas de '/', caractères whitelist */
static int safe_static_path(const char *p)
{
	if (strstr(p, "..")) return 0;
	for (const char *c = p; *c; c++) {
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
		      (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' ||
		      *c == '-' || *c == '/'))
			return 0;
	}
	return 1;
}

/* ============================== POST handling ===================== */
/* libmicrohttpd appelle handler plusieurs fois pour un POST : il faut
 * accumuler le body dans un buffer attaché à *con_cls. */

struct post_buf {
	char   data[POST_MAX_BYTES];
	size_t len;
};

/* ============================== ALSA amixer helpers (E7.3b) ======= */

#define ALSA_CARD "softac5212tdm"

/* Validate value string : whitelist [0-9a-zA-Z .,_-]. Avoid shell injection
 * even if we use fork+exec without shell — defense in depth. Limit 256 chars
 * to accommodate BYTES blobs (e.g. TAC5212 biquad coefs : 20 decimal bytes
 * comma-separated ≈ 80 chars). */
static int amixer_value_safe(const char *v)
{
	if (!v || !*v) return 0;
	for (const char *p = v; *p; p++) {
		char c = *p;
		if (!(isalnum((unsigned char)c) || c == ' ' || c == '.' ||
		      c == ',' || c == '-' || c == '_'))
			return 0;
	}
	return strlen(v) < 256;
}

/* Run `amixer -c softac5212tdm contents` and capture stdout into out[cap].
 * Returns bytes read on success, -1 on error. */
static int run_amixer_contents(char *out, size_t cap)
{
	int fds[2];
	if (pipe(fds) < 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
		execlp("amixer", "amixer", "-c", ALSA_CARD, "contents", (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	size_t total = 0;
	ssize_t n;
	while (total < cap - 1 &&
	       (n = read(fds[0], out + total, cap - 1 - total)) > 0)
		total += (size_t)n;
	out[total] = 0;
	close(fds[0]);
	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return (int)total;
}

/* Run `amixer -c softac5212tdm cset numid=<numid> <value>`.
 * Returns 0 on success, -1 on error. */
static int run_amixer_cset(int numid, const char *value)
{
	if (numid <= 0 || !amixer_value_safe(value))
		return -1;
	char numid_arg[32];
	snprintf(numid_arg, sizeof(numid_arg), "numid=%d", numid);
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execlp("amixer", "amixer", "-c", ALSA_CARD, "cset",
		       numid_arg, value, (char *)NULL);
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Run /usr/bin/tac-reset <mode> serialized two ways :
 *   - pthread_mutex : guards concurrent GUI clicks within this process
 *   - fcntl F_SETLK on /run/tac-reset.lock : guards against systemd
 *     ExecStartPre racing the GUI in another process
 * Mode is whitelisted to avoid arg injection.
 * Returns 0 on success, -1 if locked/timeout/exec failure, exit code in *exitp. */
static pthread_mutex_t g_tac_reset_mu = PTHREAD_MUTEX_INITIALIZER;

static int run_tac_reset(const char *mode, int *exitp)
{
	if (!mode || (strcmp(mode, "analog") != 0 && strcmp(mode, "pdm") != 0))
		return -1;

	if (pthread_mutex_trylock(&g_tac_reset_mu) != 0) {
		errno = EBUSY;
		return -1;
	}

	int lockfd = open("/run/tac-reset.lock",
			  O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lockfd < 0) {
		mlog("tac-reset: lock open err=%d (%s)", errno, strerror(errno));
		pthread_mutex_unlock(&g_tac_reset_mu);
		return -1;
	}
	struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	if (fcntl(lockfd, F_SETLK, &fl) < 0) {
		mlog("tac-reset: cross-process lock held");
		close(lockfd);
		pthread_mutex_unlock(&g_tac_reset_mu);
		errno = EBUSY;
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(lockfd);
		pthread_mutex_unlock(&g_tac_reset_mu);
		return -1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execl("/usr/bin/tac-reset", "tac-reset", mode, (char *)NULL);
		_exit(127);
	}

	int status = 0;
	int done = 0;
	for (int i = 0; i < 80; i++) { /* 8 s, 100 ms tick */
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) { done = 1; break; }
		if (r < 0)    { break; }
		usleep(100000);
	}
	if (!done) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		close(lockfd);
		pthread_mutex_unlock(&g_tac_reset_mu);
		mlog("tac-reset: timeout, killed pid=%d", pid);
		errno = ETIMEDOUT;
		return -1;
	}
	close(lockfd);
	pthread_mutex_unlock(&g_tac_reset_mu);
	if (exitp) *exitp = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ============================== SOF TLV-byte helpers (E7.4.c) =====
 * Read/write SOF "bytes_ext" controls (used for MULTIBAND_DRC / DRC
 * config blobs). amixer cget/cset don't handle TLV byte controls
 * ("skipping bytes dump"), so we go through alsa-lib snd_ctl APIs +
 * the TLV ioctl underneath.
 *
 * SOF wraps its blobs in a 32-byte ABI header (struct sof_abi_hdr)
 * followed by the actual payload, and the TLV wrapper itself prefixes
 * everything with (tag, size). The full buffer layout retrieved by
 * snd_ctl_elem_tlv_read() is :
 *
 *   tlv[0]   = ASoC TLV tag (SOF defines SOF_CTRL_TLV_DATA = 0x1004)
 *   tlv[1]   = total payload size in bytes (NOT including these 8 hdr bytes)
 *   tlv[2..] = payload = struct sof_abi_hdr (32 B) + blob bytes
 */

/* Read TLV-byte control by numid into `out` (up to cap bytes). Returns the
 * payload size on success, -1 on error. The returned bytes include the SOF
 * ABI header so the caller / userspace can identify the version + blob type. */
static int sof_blob_read(int numid, unsigned char *out, size_t cap)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *eid;
	unsigned int *tlv = NULL;
	size_t tlv_size;
	int ret;

	int oc = snd_ctl_open(&ctl, "hw:" ALSA_CARD, 0);
	if (oc < 0) {
		mlog("sof_blob_read: snd_ctl_open(hw:%s) err=%d (%s)",
		     ALSA_CARD, oc, snd_strerror(oc));
		return -1;
	}

	snd_ctl_elem_id_alloca(&eid);
	snd_ctl_elem_id_set_numid(eid, numid);

	/* Allocate room for 8B header + max payload (clamp to 8 KB) */
	tlv_size = (cap + 16 + 3) & ~3;
	if (tlv_size > 8192)
		tlv_size = 8192;
	tlv = calloc(1, tlv_size);
	if (!tlv) { snd_ctl_close(ctl); return -1; }

	int rc = snd_ctl_elem_tlv_read(ctl, eid, tlv, tlv_size);
	if (rc < 0) {
		mlog("sof_blob_read: numid=%d tlv_read err=%d (%s)",
		     numid, rc, snd_strerror(rc));
		free(tlv);
		snd_ctl_close(ctl);
		return -1;
	}
	mlog("sof_blob_read: numid=%d tlv ok, tag=0x%x size=%u",
	     numid, tlv[0], tlv[1]);

	unsigned int payload = tlv[1];
	if (payload > cap) payload = cap;
	memcpy(out, &tlv[2], payload);
	ret = (int)payload;

	free(tlv);
	snd_ctl_close(ctl);
	return ret;
}

/* Write a payload to a TLV-byte control. `data` is the full payload
 * including SOF ABI header. Returns 0 on success, -1 on error. */
static int sof_blob_write(int numid, const unsigned char *data, size_t size)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *eid;
	unsigned int *tlv = NULL;
	size_t tlv_size;
	int ret;

	if (!data || size == 0 || size > 8000) return -1;
	if (snd_ctl_open(&ctl, "hw:" ALSA_CARD, 0) < 0)
		return -1;

	snd_ctl_elem_id_alloca(&eid);
	snd_ctl_elem_id_set_numid(eid, numid);

	tlv_size = ((size + 8 + 3) & ~3);
	tlv = calloc(1, tlv_size);
	if (!tlv) { snd_ctl_close(ctl); return -1; }

	/* SOF kernel side (ipc3-control.c::snd_sof_bytes_ext_put) verifies
	 * that header.numid == scontrol->cmd. For bytes_ext blob kcontrols
	 * scontrol->cmd == SOF_CTRL_CMD_BINARY == 3. Reusing 0x1004 makes
	 * the kernel reject the write with -EINVAL. */
	tlv[0] = 3;            /* SOF_CTRL_CMD_BINARY */
	tlv[1] = (unsigned int)size;
	memcpy(&tlv[2], data, size);

	ret = snd_ctl_elem_tlv_write(ctl, eid, tlv);

	free(tlv);
	snd_ctl_close(ctl);
	return ret < 0 ? -1 : 0;
}

/* Hex-encode `size` bytes from `src` into a NUL-terminated string in `dst`,
 * which must hold at least `2*size + 1` chars. */
static void hex_encode(const unsigned char *src, size_t size, char *dst)
{
	static const char H[] = "0123456789abcdef";
	for (size_t i = 0; i < size; i++) {
		dst[2*i]     = H[(src[i] >> 4) & 0xf];
		dst[2*i + 1] = H[ src[i]       & 0xf];
	}
	dst[2*size] = 0;
}

/* Decode hex string `src` into bytes in `dst`. Returns nb bytes decoded,
 * or -1 on malformed input. */
static int hex_decode(const char *src, unsigned char *dst, size_t cap)
{
	size_t n = strlen(src);
	if (n & 1) return -1;
	n /= 2;
	if (n > cap) return -1;
	for (size_t i = 0; i < n; i++) {
		int hi = src[2*i], lo = src[2*i + 1];
		hi = (hi >= '0' && hi <= '9') ? hi - '0'
		    : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
		    : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
		lo = (lo >= '0' && lo <= '9') ? lo - '0'
		    : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
		    : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
		if (hi < 0 || lo < 0) return -1;
		dst[i] = (unsigned char)((hi << 4) | lo);
	}
	return (int)n;
}

/* Minimal JSON helpers : extract "key":<int> or "key":"<str>" from a JSON line. */
static int json_get_int_field(const char *s, const char *key, int *out)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
	if (!*p) return -1;
	char *end;
	long v = strtol(p, &end, 10);
	if (end == p) return -1;
	*out = (int)v;
	return 0;
}
static int json_get_str_field(const char *s, const char *key, char *out, size_t cap)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(s, pattern);
	if (!p) return -1;
	p += strlen(pattern);
	while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
	if (*p != '"') return -1;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
	out[i] = 0;
	return (*p == '"') ? 0 : -1;
}

/* ============================== Request handler =================== */

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn,
				  const char *url, const char *method,
				  const char *version, const char *upload_data,
				  size_t *upload_data_size, void **con_cls)
{
	(void)cls; (void)version;

	/* OPTIONS preflight CORS */
	if (!strcmp(method, "OPTIONS"))
		return send_text(conn, 204, "text/plain", "", 0);

	/* === Routes GET === */
	if (!strcmp(method, "GET")) {
		if (!strcmp(url, "/") || !strcmp(url, "/index.html"))
			return send_file(conn, WWW_ROOT "/index.html", "text/html; charset=utf-8");
		/* V10-P0 : nouveau design en preview ; /panel = cible kiosk 1024x600 */
		if (!strcmp(url, "/beta") || !strcmp(url, "/panel"))
			return send_file(conn, WWW_ROOT "/beta.html", "text/html; charset=utf-8");

		if (!strcmp(url, "/health")) {
			char body[128];
			snprintf(body, sizeof(body),
				 "{\"ok\":true,\"version\":\"%s\"}\n", GUI_VERSION);
			return send_json(conn, 200, body);
		}

		if (!strcmp(url, "/api/state")) {
			char reply[8192];
			int n = mixer_request("{\"op\":\"get_state\"}\n", reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/meters")) {
			/* E7.1 : REST polling fallback / debug curl */
			char reply[2048];
			int n = mixer_request("{\"op\":\"get_meters\"}\n", reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/sysload")) {
			/* V10-P1 : source UNIQUE = le producteur state (1 Hz) —
			 * plus de double fenêtre de mesure (finding critic it.2). */
			char out[256];
			pthread_mutex_lock(&g_sysload_mu);
			snprintf(out, sizeof(out), "%s",
				 g_sysload_json[0] ? g_sysload_json
				 : "{\"ok\":false,\"err\":\"warming up\"}");
			pthread_mutex_unlock(&g_sysload_mu);
			return send_json(conn, 200, out);
		}

		if (!strcmp(url, "/api/drift")) {
			/* V8.1.b : drift USB↔DSP mesuré passivement par mixer-pro.
			 * V8.30 : grand buffer pour les stats timing wr/rd. */
			char reply[1024];
			int n = mixer_request("{\"op\":\"get_drift\"}\n", reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/reset_drift_stats")) {
			/* V8.12 : reset shift_ppm + drift + tous les compteurs ring */
			char reply[128];
			int n = mixer_request("{\"op\":\"reset_drift_stats\"}\n",
			                      reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/apply_drift_as_shift")) {
			/* V8.14 : force shift_ppm = round(drift_ppm) */
			char reply[160];
			int n = mixer_request("{\"op\":\"apply_drift_as_shift\"}\n",
			                      reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/state/sse")) {
			/* V10-P1 : flux d'état versionné (full + patches par section).
			 * ?panel=1 → slot 0 réservé au kiosk (jamais 503). */
			const char *pv = MHD_lookup_connection_value(conn,
					MHD_GET_ARGUMENT_KIND, "panel");
			struct sclient *c = sc_alloc(pv && !strcmp(pv, "1"));
			if (!c) {
				struct MHD_Response *r503 = MHD_create_response_from_buffer(
					35, "{\"ok\":false,\"err\":\"sse slots full\"}",
					MHD_RESPMEM_MUST_COPY);
				MHD_add_response_header(r503, "Retry-After", "5");
				add_cors_headers(r503);
				enum MHD_Result rr = MHD_queue_response(conn, 503, r503);
				MHD_destroy_response(r503);
				return rr;
			}
			struct MHD_Response *r = MHD_create_response_from_callback(
				MHD_SIZE_UNKNOWN, 65536, &sse_state_cb, c, &sse_state_free);
			if (!r) { sse_state_free(c); return MHD_NO; }
			MHD_add_response_header(r, "Content-Type", "text/event-stream");
			MHD_add_response_header(r, "Cache-Control", "no-cache");
			MHD_add_response_header(r, "Connection", "keep-alive");
			MHD_add_response_header(r, "X-Accel-Buffering", "no");
			add_cors_headers(r);
			enum MHD_Result ret = MHD_queue_response(conn, 200, r);
			MHD_destroy_response(r);
			return ret;
		}

		if (!strcmp(url, "/api/state/full")) {
			/* resync à la demande (gap de seq côté client) */
			/* V10-P2g : buffer par requête (le static était partagé
			 * entre threads MHD → réponses corrompues à 2 clients). */
			char *full = malloc(SC_FRAME);
			if (!full) return MHD_NO;
			int n = sc_build_full(full, SC_FRAME);
			/* strip "data: " et le \n\n final pour renvoyer du JSON pur */
			enum MHD_Result ret;
			if (n > 8)
				ret = send_text(conn, 200, "application/json",
						full + 6, (size_t)(n - 8));
			else
				ret = send_json(conn, 503, "{\"ok\":false,\"err\":\"state not ready\"}\n");
			free(full);
			return ret;
		}

		if (!strcmp(url, "/api/debug/sse")) {
			char dbg[512];
			int n = snprintf(dbg, sizeof(dbg),
				"{\"ok\":true,\"seq\":%u,\"frames_out\":%lu,\"clients\":[",
				g_sseq, g_sc_frames_out);
			int first = 1;
			for (int i = 0; i < SC_MAX; i++)
				if (g_sc[i].used) {
					n += snprintf(dbg + n, sizeof(dbg) - n,
						"%s{\"slot\":%d,\"panel\":%d,\"drops\":%d,\"backlog\":%u}",
						first ? "" : ",", i, g_sc[i].is_panel,
						g_sc[i].drops, g_sc[i].wr - g_sc[i].rd);
					first = 0;
				}
			n += snprintf(dbg + n, sizeof(dbg) - n, "]}");
			return send_json(conn, 200, dbg);
		}

		if (!strcmp(url, "/api/stream")) {
			/* E7.1 + E7.5 : SSE meters+analyzer stream 30 Hz.
			 * Block size bumped to 32 KB so the analyzer payload (~6 KB
			 * JSON for 4 taps with 128 spec + 128 scope ints) plus the
			 * SSE framing fits in a single callback invocation. */
			struct MHD_Response *r = MHD_create_response_from_callback(
				MHD_SIZE_UNKNOWN, 32768, &sse_stream_callback, NULL, NULL);
			if (!r) return MHD_NO;
			MHD_add_response_header(r, "Content-Type", "text/event-stream");
			MHD_add_response_header(r, "Cache-Control", "no-cache");
			MHD_add_response_header(r, "Connection", "keep-alive");
			MHD_add_response_header(r, "X-Accel-Buffering", "no");
			add_cors_headers(r);
			enum MHD_Result ret = MHD_queue_response(conn, 200, r);
			MHD_destroy_response(r);
			return ret;
		}

		if (!strcmp(url, "/api/alsa/contents")) {
			/* E7.3b : dump all ALSA kcontrols of softac5212tdm card.
			 * Returns raw amixer -c <card> contents output as text/plain.
			 * Parsed client-side (Alpine.js) into typed controls.
			 */
			/* V10-P2g : buffer par requête — le static partagé entre
			 * threads MHD corrompait la réponse dès 2 GUIs ouvertes
			 * (ancienne + beta) → parse client sans contrôles TAC. */
			char *buf = malloc(64 * 1024);
			if (!buf) return MHD_NO;
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			int n = run_amixer_contents(buf, 64 * 1024);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			{	/* V10-P2h : trace chaque hit (diagnostic panne distante) */
				const union MHD_ConnectionInfo *ci = MHD_get_connection_info(
					conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
				char ip[64] = "?";
				if (ci && ci->client_addr && ci->client_addr->sa_family == AF_INET)
					inet_ntop(AF_INET,
						  &((struct sockaddr_in *)ci->client_addr)->sin_addr,
						  ip, sizeof(ip));
				long ms = (t1.tv_sec - t0.tv_sec) * 1000 +
					  (t1.tv_nsec - t0.tv_nsec) / 1000000;
				mlog("alsa/contents: client=%s n=%d ms=%ld", ip, n, ms);
			}
			enum MHD_Result ret;
			if (n < 0)
				ret = send_json(conn, 503,
					"{\"ok\":false,\"err\":\"amixer contents failed\"}\n");
			else
				ret = send_text(conn, 200, "text/plain; charset=utf-8",
						buf, (size_t)n);
			free(buf);
			return ret;
		}

		if (!strncmp(url, "/static/", 8)) {
			const char *rel = url + 1;  /* "static/..." */
			if (!safe_static_path(rel))
				return send_json(conn, 400, "{\"ok\":false,\"err\":\"bad path\"}\n");
			char full[512];
			snprintf(full, sizeof(full), "%s/%s", WWW_ROOT, rel);
			return send_file(conn, full, guess_mime(full));
		}

		/* === Route GET /api/dsp/blob/<numid>/raw === (E7.4.c)
		 * Returns the raw SOF blob (incl ABI header) as hex.
		 * Reply : {"ok",numid,size,hex} */
		if (!strncmp(url, "/api/dsp/blob/", 14)) {
			const char *p = url + 14;
			char *endp = NULL;
			long numid = strtol(p, &endp, 10);
			if (numid <= 0 || !endp || strcmp(endp, "/raw") != 0)
				return send_json(conn, 400,
					"{\"ok\":false,\"err\":\"bad numid path\"}\n");
			unsigned char buf[6000];   /* pile : par requête (V10-P2g) */
			int sz = sof_blob_read((int)numid, buf, sizeof(buf));
			if (sz < 0)
				return send_json(conn, 503,
					"{\"ok\":false,\"err\":\"tlv read failed\"}\n");
			char hex[12100];
			hex_encode(buf, (size_t)sz, hex);
			char body[12300];
			snprintf(body, sizeof(body),
				"{\"ok\":true,\"numid\":%ld,\"size\":%d,\"hex\":\"%s\"}\n",
				numid, sz, hex);
			return send_json(conn, 200, body);
		}

		return send_json(conn, MHD_HTTP_NOT_FOUND, "{\"ok\":false,\"err\":\"not found\"}\n");
	}

	/* === Route POST /api/cmd === */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/cmd")) {
		struct post_buf *pb = *con_cls;
		if (!pb) {
			pb = calloc(1, sizeof(*pb));
			if (!pb) return MHD_NO;
			*con_cls = pb;
			return MHD_YES;  /* attente du body */
		}

		if (*upload_data_size > 0) {
			size_t avail = POST_MAX_BYTES - 1 - pb->len;
			size_t n = *upload_data_size < avail ? *upload_data_size : avail;
			memcpy(pb->data + pb->len, upload_data, n);
			pb->len += n;
			pb->data[pb->len] = '\0';
			*upload_data_size = 0;
			return MHD_YES;
		}

		/* End of upload : forward */
		if (pb->len == 0)
			return send_json(conn, 400, "{\"ok\":false,\"err\":\"empty body\"}\n");

		/* Ajouter \n si manquant */
		char req[POST_MAX_BYTES + 2];
		size_t n = pb->len;
		memcpy(req, pb->data, n);
		if (req[n - 1] != '\n') req[n++] = '\n';
		req[n] = '\0';

		/* V9.2d-step5e : 32 KB pour list_lv2_plugins avec 200+ plugins.
		 * V9.5.21 : 64 KB — l'ajout des champs cat/ai/ao/ins par plugin a
		 * porté la liste (345 plugins) à ~42 KB → 32 KB tronquait le JSON
		 * → 0 effet dans la GUI. Aligné sur le lv2_buf 65536 de mixer-pro. */
		/* V10-P2g : par requête — le static croisait les réponses
		 * de tous les clients POST simultanés. */
		char *reply = malloc(65536);
		if (!reply) return MHD_NO;
		int rc = mixer_request(req, reply, 65536);
		enum MHD_Result ret = send_json(conn, rc > 0 ? 200 : 503, reply);
		free(reply);
		return ret;
	}

	/* === Route POST /api/client-log === (V10-P2h diag)
	 * Le front remonte ses erreurs fetch/JS ici (sendBeacon) — évite le
	 * copier-coller utilisateur pour diagnostiquer les pannes distantes. */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/client-log")) {
		struct post_buf *pb = *con_cls;
		if (!pb) {
			pb = calloc(1, sizeof(*pb));
			if (!pb) return MHD_NO;
			*con_cls = pb;
			return MHD_YES;
		}
		if (*upload_data_size > 0) {
			size_t room = POST_MAX_BYTES - pb->len;
			size_t take = *upload_data_size < room ? *upload_data_size : room;
			memcpy(pb->data + pb->len, upload_data, take);
			pb->len += take;
			*upload_data_size = 0;
			return MHD_YES;
		}
		pb->data[pb->len < POST_MAX_BYTES ? pb->len : POST_MAX_BYTES - 1] = 0;
		const union MHD_ConnectionInfo *ci = MHD_get_connection_info(
			conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
		char ip[64] = "?";
		if (ci && ci->client_addr && ci->client_addr->sa_family == AF_INET)
			inet_ntop(AF_INET,
				  &((struct sockaddr_in *)ci->client_addr)->sin_addr,
				  ip, sizeof(ip));
		mlog("client-log %s: %.300s", ip, pb->data);
		return send_json(conn, 200, "{\"ok\":true}\n");
	}

	/* === Route POST /api/alsa/set === (E7.3b)
	 * Body : {"numid": <int>, "value": "<value-string>"}
	 * Calls : amixer -c softac5212tdm cset numid=<N> "<value>"
	 */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/alsa/set")) {
		struct post_buf *pb = *con_cls;
		if (!pb) {
			pb = calloc(1, sizeof(*pb));
			if (!pb) return MHD_NO;
			*con_cls = pb;
			return MHD_YES;
		}
		if (*upload_data_size > 0) {
			size_t avail = POST_MAX_BYTES - 1 - pb->len;
			size_t n = *upload_data_size < avail ? *upload_data_size : avail;
			memcpy(pb->data + pb->len, upload_data, n);
			pb->len += n;
			pb->data[pb->len] = '\0';
			*upload_data_size = 0;
			return MHD_YES;
		}
		if (pb->len == 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"empty body\"}\n");
		int numid = 0;
		char value[256];
		if (json_get_int_field(pb->data, "numid", &numid) < 0 ||
		    json_get_str_field(pb->data, "value", value, sizeof(value)) < 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"need numid + value\"}\n");
		int r = run_amixer_cset(numid, value);
		if (r != 0)
			return send_json(conn, 503,
				"{\"ok\":false,\"err\":\"amixer cset failed\"}\n");
		char reply[320];
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"numid\":%d,\"value\":\"%s\"}\n",
			 numid, value);
		return send_json(conn, 200, reply);
	}

	/* === Route POST /api/dsp/blob/set === (E7.4.c)
	 * Body : {"numid":N, "hex":"<bytes>"} where hex = full SOF payload
	 *        (ABI header + blob bytes).
	 */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/dsp/blob/set")) {
		struct post_buf *pb = *con_cls;
		if (!pb) {
			pb = calloc(1, sizeof(*pb));
			if (!pb) return MHD_NO;
			*con_cls = pb;
			return MHD_YES;
		}
		if (*upload_data_size > 0) {
			size_t avail = POST_MAX_BYTES - 1 - pb->len;
			size_t n = *upload_data_size < avail ? *upload_data_size : avail;
			memcpy(pb->data + pb->len, upload_data, n);
			pb->len += n;
			pb->data[pb->len] = '\0';
			*upload_data_size = 0;
			return MHD_YES;
		}
		if (pb->len == 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"empty body\"}\n");
		int numid = 0;
		const char *p = strstr(pb->data, "\"hex\"");
		if (json_get_int_field(pb->data, "numid", &numid) < 0 || !p)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"need numid + hex\"}\n");
		/* Walk past 3 quotes : open"hex" close"hex" open"<value>" — landing
		 * on the first char of the hex string. */
		p = strchr(p, '"');
		if (p) p = strchr(p + 1, '"');
		if (p) p = strchr(p + 1, '"');
		const char *hex_start = p ? p + 1 : NULL;
		const char *hex_end = hex_start ? strchr(hex_start, '"') : NULL;
		if (!hex_start || !hex_end || hex_end <= hex_start)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"bad hex field\"}\n");
		size_t hex_len = (size_t)(hex_end - hex_start);
		char hex[12100];           /* pile : par requête (V10-P2g) */
		if (hex_len >= sizeof(hex))
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"hex too large\"}\n");
		memcpy(hex, hex_start, hex_len); hex[hex_len] = 0;
		unsigned char buf[6000];
		int sz = hex_decode(hex, buf, sizeof(buf));
		if (sz < 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"malformed hex\"}\n");
		if (sof_blob_write(numid, buf, (size_t)sz) != 0)
			return send_json(conn, 503,
				"{\"ok\":false,\"err\":\"tlv write failed\"}\n");
		char reply[128];
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"numid\":%d,\"size\":%d}\n", numid, sz);
		return send_json(conn, 200, reply);
	}

	/* === Route POST /api/tac/reset === (E7.4.f)
	 * Body : {"mode":"analog|pdm"} (optional, default "analog")
	 * Runs /usr/bin/tac-reset, guarded by flock so it can't race
	 * with systemd ExecStartPre on the mixer-pro service.
	 */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/tac/reset")) {
		struct post_buf *pb = *con_cls;
		if (!pb) {
			pb = calloc(1, sizeof(*pb));
			if (!pb) return MHD_NO;
			*con_cls = pb;
			return MHD_YES;
		}
		if (*upload_data_size > 0) {
			size_t avail = POST_MAX_BYTES - 1 - pb->len;
			size_t n = *upload_data_size < avail ? *upload_data_size : avail;
			memcpy(pb->data + pb->len, upload_data, n);
			pb->len += n;
			pb->data[pb->len] = '\0';
			*upload_data_size = 0;
			return MHD_YES;
		}
		char mode[16] = "analog";
		if (pb->len > 0)
			(void)json_get_str_field(pb->data, "mode", mode, sizeof(mode));
		if (strcmp(mode, "analog") != 0 && strcmp(mode, "pdm") != 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"mode must be analog or pdm\"}\n");

		int exit_code = -1;
		int r = run_tac_reset(mode, &exit_code);
		if (r != 0) {
			const char *why = "tac-reset failed";
			int http = 503;
			if (errno == EBUSY)    { why = "tac-reset already running"; http = 409; }
			if (errno == ETIMEDOUT) { why = "tac-reset timeout";        http = 504; }
			char reply[160];
			snprintf(reply, sizeof(reply),
				 "{\"ok\":false,\"err\":\"%s\",\"exit\":%d}\n",
				 why, exit_code);
			return send_json(conn, http, reply);
		}
		char reply[128];
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"mode\":\"%s\",\"exit\":%d}\n",
			 mode, exit_code);
		return send_json(conn, 200, reply);
	}

	return send_json(conn, MHD_HTTP_NOT_FOUND, "{\"ok\":false,\"err\":\"not found\"}\n");
}

static void on_request_completed(void *cls, struct MHD_Connection *conn,
				 void **con_cls, enum MHD_RequestTerminationCode toe)
{
	(void)cls; (void)conn; (void)toe;
	if (*con_cls) {
		free(*con_cls);
		*con_cls = NULL;
	}
}

/* ============================== Main ============================== */

static volatile int g_stop = 0;
static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

int main(int argc, char **argv)
{
	int port = DEFAULT_PORT;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--help")) {
			fprintf(stderr,
				"usage: %s [-p <port>]  (default port: 8080)\n",
				argv[0]);
			return 0;
		}
	}

	mlog("mixer-gui-http %s starting on port %d", GUI_VERSION, port);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	/* V10-P1 : clients state stream + thread producteur */
	for (int i = 0; i < SC_MAX; i++) {
		pthread_mutex_init(&g_sc[i].mu, NULL);
		pthread_cond_init(&g_sc[i].cv, NULL);
	}
	pthread_t state_th;
	pthread_create(&state_th, NULL, state_producer, NULL);

	struct MHD_Daemon *d = MHD_start_daemon(
		MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_AUTO,
		port, NULL, NULL,
		&on_request, NULL,
		MHD_OPTION_THREAD_POOL_SIZE, (unsigned)MHD_THREAD_POOL,
		MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)30,
		MHD_OPTION_NOTIFY_COMPLETED, &on_request_completed, NULL,
		MHD_OPTION_END);
	if (!d) {
		mlog("ERROR: MHD_start_daemon failed (port %d busy?)", port);
		return 1;
	}

	mlog("ready : http://0.0.0.0:%d/  (thread pool %d, connect-per-req)",
	     port, MHD_THREAD_POOL);

	while (!g_stop)
		pause();

	mlog("shutdown");
	MHD_stop_daemon(d);
	return 0;
}
