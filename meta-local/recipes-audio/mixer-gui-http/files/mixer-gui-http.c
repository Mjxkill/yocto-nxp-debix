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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <microhttpd.h>

#define GUI_VERSION       "v7.0-e7.3b"
#define DEFAULT_PORT      8080
#define MIXER_SOCK_PATH   "/run/mixer-pro.sock"
#define WWW_ROOT          "/var/www/mixer-gui"
#define POST_MAX_BYTES    4096
#define SOCK_RECV_TIMEO_MS 200
#define MHD_THREAD_POOL   8       /* E7.1 : 4 SSE persistants + 4 REST/static */
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
	struct timeval tv = { 0, SOCK_RECV_TIMEO_MS * 1000 };
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

	char meters_json[2048];
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
 * even if we use fork+exec without shell — defense in depth. */
static int amixer_value_safe(const char *v)
{
	if (!v || !*v) return 0;
	for (const char *p = v; *p; p++) {
		char c = *p;
		if (!(isalnum((unsigned char)c) || c == ' ' || c == '.' ||
		      c == ',' || c == '-' || c == '_'))
			return 0;
	}
	return strlen(v) < 64;
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

		if (!strcmp(url, "/api/stream")) {
			/* E7.1 : SSE meters stream 30 Hz (chunked callback) */
			struct MHD_Response *r = MHD_create_response_from_callback(
				MHD_SIZE_UNKNOWN, 4096, &sse_stream_callback, NULL, NULL);
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
			static char buf[64 * 1024];
			int n = run_amixer_contents(buf, sizeof(buf));
			if (n < 0)
				return send_json(conn, 503,
					"{\"ok\":false,\"err\":\"amixer contents failed\"}\n");
			return send_text(conn, 200, "text/plain; charset=utf-8",
					 buf, (size_t)n);
		}

		if (!strncmp(url, "/static/", 8)) {
			const char *rel = url + 1;  /* "static/..." */
			if (!safe_static_path(rel))
				return send_json(conn, 400, "{\"ok\":false,\"err\":\"bad path\"}\n");
			char full[512];
			snprintf(full, sizeof(full), "%s/%s", WWW_ROOT, rel);
			return send_file(conn, full, guess_mime(full));
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

		char reply[8192];
		int rc = mixer_request(req, reply, sizeof(reply));
		return send_json(conn, rc > 0 ? 200 : 503, reply);
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
		char value[64];
		if (json_get_int_field(pb->data, "numid", &numid) < 0 ||
		    json_get_str_field(pb->data, "value", value, sizeof(value)) < 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"need numid + value\"}\n");
		int r = run_amixer_cset(numid, value);
		if (r != 0)
			return send_json(conn, 503,
				"{\"ok\":false,\"err\":\"amixer cset failed\"}\n");
		char reply[128];
		snprintf(reply, sizeof(reply),
			 "{\"ok\":true,\"numid\":%d,\"value\":\"%s\"}\n",
			 numid, value);
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
