// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mixer_bridge — requête JSON ligne-par-ligne vers /run/mixer-pro.sock
 * (connect-per-request, timeout). Déplacé tel quel (V14.0 étape 6).
 */
#include "gui_http.h"

/* ============================== Mixer-pro socket bridge ============= */
/* Note : mixer-pro control_thread fait `accept → read → handle → close` séquentiel.
 * Un pool de sockets persistants côté GUI ne marche pas (close passif côté mixer-pro
 * → CLOSE_WAIT silencieux → read timeout sur la 2e requête du même socket).
 * On fait donc un connect/close par requête. Overhead acceptable : ~1-2 ms par
 * requête × 2 Hz polling × 1-5 clients = 2-10 ms/s CPU sur control_thread. */

int mixer_request(const char *req_line, char *out, size_t out_sz)
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

