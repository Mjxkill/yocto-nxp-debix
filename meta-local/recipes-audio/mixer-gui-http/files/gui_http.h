// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gui_http — header INTERNE du daemon mixer-gui-http (V14.0 étape 6) :
 * defines partagés + protos entre les 5 modules. Découpage :
 *   http_core.c    main + MHD + helpers HTTP + mlog
 *   mixer_bridge.c socket JSON vers mixer-pro
 *   sse_state.c    flux d'état versionné 30 Hz + SSE + sysload
 *   alsa_ctl.c     amixer + tac-reset + blobs TLV SOF + alsactl store
 *   api_routes.c   on_request (routage GET/POST complet)
 */
#ifndef GUI_HTTP_H
#define GUI_HTTP_H

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
#include <stdatomic.h>
#include <microhttpd.h>
#include <alsa/asoundlib.h>


#define GUI_VERSION       "v8.1b-drift-meter"
/* V10-N7b : miroir des blobs DSP appliqués (rejoués au boot) */
#define DSP_BLOB_DIR      "/var/lib/mixer-pro/dsp-blobs"


extern _Atomic int g_alsa_dirty;

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


extern unsigned long g_sc_frames_out;   /* frames SSE émises (health) */
extern unsigned g_sseq;                 /* n° de séquence du flux état */

/* POST accumulé sur plusieurs appels MHD (attaché à *con_cls) */
struct post_buf {
	char   data[POST_MAX_BYTES];
	size_t len;
};

void mlog(const char *fmt, ...);
int  mixer_request(const char *req_line, char *out, size_t out_sz);

ssize_t sse_stream_callback(void *cls, uint64_t pos, char *buf, size_t max);
void   *state_producer(void *arg);
ssize_t sse_state_cb(void *cls, uint64_t pos, char *buf, size_t max);
void    sse_state_free(void *cls);
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
extern struct sclient g_sc[SC_MAX];
extern char g_sysload_json[256];
extern pthread_mutex_t g_sysload_mu;

struct sclient *sc_alloc(int is_panel);
int sc_build_full(char *out, size_t sz);   /* snapshot complet (reconnexion SSE) */

void add_cors_headers(struct MHD_Response *r);
enum MHD_Result send_text(struct MHD_Connection *conn, int code,
			  const char *ctype, const char *body, size_t len);
enum MHD_Result send_json(struct MHD_Connection *conn, int code, const char *body);
enum MHD_Result send_file(struct MHD_Connection *conn, const char *path,
			  const char *ctype);
const char *guess_mime(const char *path);
int safe_static_path(const char *p);

void *alsa_store_thread(void *arg);
int amixer_value_safe(const char *v);
int run_amixer_contents(char *out, size_t cap);
int run_amixer_cset(int numid, const char *value);
int run_tac_reset(const char *mode, int *exitp);
int sof_blob_read(int numid, unsigned char *out, size_t cap);
int sof_blob_write(int numid, const unsigned char *data, size_t size);
void hex_encode(const unsigned char *src, size_t size, char *dst);
int  hex_decode(const char *src, unsigned char *dst, size_t cap);
int  json_get_int_field(const char *s, const char *key, int *out);
int  json_get_str_field(const char *s, const char *key, char *out, size_t cap);

enum MHD_Result on_request(void *cls, struct MHD_Connection *conn,
			   const char *url, const char *method,
			   const char *version, const char *upload_data,
			   size_t *upload_data_size, void **con_cls);
void on_request_completed(void *cls, struct MHD_Connection *conn,
			  void **con_cls, enum MHD_RequestTerminationCode toe);

#endif /* GUI_HTTP_H */
