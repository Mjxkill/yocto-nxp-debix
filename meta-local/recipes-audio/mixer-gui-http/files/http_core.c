// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * http_core — main : démarrage MHD (pool 16), signaux, helpers
 * HTTP (CORS, send_*, mime, chemins statiques), mlog.
 * Déplacé tel quel (V14.0 étape 6).
 */
#include "gui_http.h"

/* libmicrohttpd 1.0.x : MHD_Result enum introduit récemment.
 * Fallback pour anciennes versions où c'était `int`. */
#ifndef MHD_HTTP_NOT_FOUND
#  define MHD_HTTP_NOT_FOUND 404
#endif

void mlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	fflush(stderr);
	va_end(ap);
}


/* ============================== HTTP helpers ====================== */

void add_cors_headers(struct MHD_Response *r)
{
	MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
	MHD_add_response_header(r, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	MHD_add_response_header(r, "Access-Control-Allow-Headers", "Content-Type");
	MHD_add_response_header(r, "Cache-Control", "no-store");
}

enum MHD_Result send_text(struct MHD_Connection *conn, int code,
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

enum MHD_Result send_json(struct MHD_Connection *conn, int code, const char *body)
{
	return send_text(conn, code, "application/json", body, strlen(body));
}

enum MHD_Result send_file(struct MHD_Connection *conn, const char *path,
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

const char *guess_mime(const char *path)
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
int safe_static_path(const char *p)
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

	/* V10-N7b : saver alsactl débouncé */
	pthread_t store_th;
	pthread_create(&store_th, NULL, alsa_store_thread, NULL);

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
