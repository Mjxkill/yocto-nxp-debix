// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * api_routes — on_request : routage GET/POST complet de l'API HTTP
 * (pages, api, SSE, TAC, blobs). Déplacé tel quel (V14.0 étape 6).
 */
#include "gui_http.h"

/* ============================== Request handler =================== */

enum MHD_Result on_request(void *cls, struct MHD_Connection *conn,
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
		/* V14.0 : beta.html = LA console (l'ancienne GUI V7 index.html est
		 * supprimée). / et les anciens alias servent tous la même page. */
		if (!strcmp(url, "/") || !strcmp(url, "/index.html") ||
		    !strcmp(url, "/beta") || !strcmp(url, "/panel"))
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
			/* E7.1 : REST polling fallback / debug curl.
			 * V13.1 : 8 KB — avec les 4 taps analyzer actifs le
			 * get_meters dépasse 2 KB → JSON tronqué (VU page SCÈNE) */
			char reply[8192];
			int n = mixer_request("{\"op\":\"get_meters\"}\n", reply, sizeof(reply));
			return send_json(conn, n > 0 ? 200 : 503, reply);
		}

		if (!strcmp(url, "/api/larsen")) {
			/* V11-AL E3 : notches live du daemon anti-larsen — son
			 * socket écrit le status dès l'accept (pas de requête). */
			char reply[2048];
			int n = -1;
			int fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (fd >= 0) {
				struct sockaddr_un sa = { .sun_family = AF_UNIX };
				strncpy(sa.sun_path, "/run/anti-larsen.sock",
					sizeof(sa.sun_path) - 1);
				struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
				if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
					ssize_t r = read(fd, reply, sizeof(reply) - 1);
					if (r > 0) { reply[r] = '\0'; n = (int)r; }
				}
				close(fd);
			}
			if (n <= 0)
				snprintf(reply, sizeof(reply),
					 "{\"ok\":false,\"err\":\"anti-larsen absent\"}\n");
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

	/* === Route POST /api/larsen === (V13-SCENES)
	 * {"enable":0|1} → commande runtime du daemon anti-larsen. */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/larsen")) {
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
		int en = strstr(pb->data, "\"enable\":1") ||
			 strstr(pb->data, "\"enable\": 1") ? 1 : 0;
		char reply[2048];
		int n = -1;
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			struct sockaddr_un sa = { .sun_family = AF_UNIX };
			strncpy(sa.sun_path, "/run/anti-larsen.sock",
				sizeof(sa.sun_path) - 1);
			struct timeval tv = { 0, 500000 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
				char cmd[24];
				int cl = snprintf(cmd, sizeof(cmd),
						  "enable %d\n", en);
				if (write(fd, cmd, (size_t)cl) == cl) {
					ssize_t r = read(fd, reply,
							 sizeof(reply) - 1);
					if (r > 0) { reply[r] = '\0'; n = (int)r; }
				}
			}
			close(fd);
		}
		if (n <= 0)
			snprintf(reply, sizeof(reply),
				 "{\"ok\":false,\"err\":\"anti-larsen absent\"}\n");
		return send_json(conn, n > 0 ? 200 : 503, reply);
	}

	/* === Routes POST /api/scene/save + /api/scene/recall === (V13-SCENES E2)
	 * Orchestration COMPLÈTE d'une scène : mixer-pro (scene_save/recall)
	 * + TAC/PGA (alsactl) + blobs DSP + patches synthé/canaux MIDI.
	 * Body : {"slot":0-5,"name":"..."} — name optionnel (save). */
	if (!strcmp(method, "POST") &&
	    (!strcmp(url, "/api/scene/save") ||
	     !strcmp(url, "/api/scene/recall"))) {
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
		int slot = -1;
		const char *sp = strstr(pb->data, "\"slot\"");
		if (sp) sscanf(sp, "\"slot\"%*[: ]%d", &slot);
		if (slot < 0 || slot > 5)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"bad slot\"}\n");
		char name[48] = "";
		const char *np = strstr(pb->data, "\"name\"");
		if (np) {
			np = strchr(np + 6, '"');
			if (np) {
				np++;
				size_t i = 0;
				while (*np && *np != '"' && i < sizeof(name) - 1)
					name[i++] = *np++;
				name[i] = '\0';
			}
		}
		char req[128], reply[512], cmd[512];
		char sdir[96];
		snprintf(sdir, sizeof(sdir),
			 "/var/lib/mixer-pro/scenes/scene%d.d", slot);
		int save = !strcmp(url, "/api/scene/save");
		if (save) {
			if (name[0])
				snprintf(req, sizeof(req),
					 "{\"op\":\"scene_save\",\"slot\":%d,"
					 "\"name\":\"%s\"}\n", slot, name);
			else
				snprintf(req, sizeof(req),
					 "{\"op\":\"scene_save\",\"slot\":%d}\n",
					 slot);
			int rc = mixer_request(req, reply, sizeof(reply));
			if (rc <= 0 || !strstr(reply, "\"ok\":true"))
				return send_json(conn, 503, reply);
			/* snapshot TAC/DSP/synthé */
			snprintf(cmd, sizeof(cmd),
				"mkdir -p %s && "
				"alsactl store -f %s/asound.state 2>/dev/null; "
				"rm -rf %s/dsp-blobs; "
				"cp -a /var/lib/mixer-pro/dsp-blobs %s/dsp-blobs 2>/dev/null; "
				"cp -a /var/lib/ala/synth-patches.conf %s/ 2>/dev/null; "
				"cp -a /var/lib/ala/midix-chans.conf %s/ 2>/dev/null; true",
				sdir, sdir, sdir, sdir, sdir, sdir);
			(void)!system(cmd);
			return send_json(conn, 200,
				"{\"ok\":true,\"op\":\"scene_save_full\"}\n");
		}
		/* recall */
		snprintf(req, sizeof(req),
			 "{\"op\":\"scene_recall\",\"slot\":%d}\n", slot);
		int rc = mixer_request(req, reply, sizeof(reply));
		if (rc <= 0 || !strstr(reply, "\"ok\":true"))
			return send_json(conn, 503, reply);
		/* TAC/PGA + blobs DSP (script : alsactl restore -f + replay) */
		snprintf(cmd, sizeof(cmd),
			"[ -d %s ] && /usr/bin/ala-fx-restore.sh %s; "
			"cp -a %s/synth-patches.conf /var/lib/ala/ 2>/dev/null; "
			"cp -a %s/midix-chans.conf /var/lib/ala/ 2>/dev/null; true",
			sdir, sdir, sdir, sdir);
		(void)!system(cmd);
		/* patches + canaux synthé rechargés (proxy mixer-pro) */
		mixer_request("{\"op\":\"midix_ctl\",\"line\":\"reload\"}\n",
			      reply, sizeof(reply));
		return send_json(conn, 200,
			"{\"ok\":true,\"op\":\"scene_recall_full\"}\n");
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
		/* V10-N7b : rémanence TAC/PGA robuste aux coupures secteur —
		 * alsactl store débouncé (le ExecStop d'alsa-restore ne couvre
		 * que les shutdowns propres) */
		atomic_store(&g_alsa_dirty, 1);
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
		/* V10-N7b : rémanence des blobs DSP (BYTES ignorés par alsactl) —
		 * miroir du hex appliqué, rejoué au boot par ala-fx-restore via
		 * ce même endpoint (chemin de code identique). Atomique. */
		mkdir(DSP_BLOB_DIR, 0755);
		char bp[128], bt[136];
		snprintf(bp, sizeof(bp), DSP_BLOB_DIR "/%d.hex", numid);
		snprintf(bt, sizeof(bt), "%s.tmp", bp);
		FILE *bf = fopen(bt, "w");
		if (bf) {
			fputs(hex, bf);
			fclose(bf);
			rename(bt, bp);
		}
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

	/* === Route POST /api/factory/reset === (V10-N7)
	 * Reset usine COMPLET : efface les états persistés de la table
	 * (/var/lib/mixer-pro : presets, mic_map, out_gain, mixer_state)
	 * puis reboote — la matrice, les gains, les inserts, le mode
	 * assistant ET les blobs DSP (mémoire DSP, défauts topology au
	 * reload firmware) reviennent aux défauts usine. Le body doit
	 * contenir {"confirm":"usine"} (garde-fou anti-appel accidentel). */
	if (!strcmp(method, "POST") && !strcmp(url, "/api/factory/reset")) {
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
		char confirm[16] = "";
		if (pb->len > 0)
			(void)json_get_str_field(pb->data, "confirm",
			                         confirm, sizeof(confirm));
		if (strcmp(confirm, "usine") != 0)
			return send_json(conn, 400,
				"{\"ok\":false,\"err\":\"confirm=usine requis\"}\n");
		fprintf(stderr, "gui-http: FACTORY RESET — purge état + reboot\n");
		/* V10-N7b : STOPPER les services persistants AVANT le rm, sinon
		 * ils re-sauvent leur état mémoire au shutdown et annulent la
		 * purge (constaté : mixer-pro final-save + alsactl store du
		 * ExecStop d'alsa-restore). systemctl stop est synchrone →
		 * les rm qui suivent sont définitifs. Détaché pour laisser la
		 * réponse HTTP partir. */
		(void)system("( systemctl stop mixer-pro alsa-restore ; "
		             "rm -rf /var/lib/mixer-pro ; "
		             "rm -f /var/lib/alsa/asound.state ; "
		             "systemctl reboot ) >/dev/null 2>&1 &");
		return send_json(conn, 200,
			"{\"ok\":true,\"msg\":\"reset usine — redémarrage\"}\n");
	}

	return send_json(conn, MHD_HTTP_NOT_FOUND, "{\"ok\":false,\"err\":\"not found\"}\n");
}

void on_request_completed(void *cls, struct MHD_Connection *conn,
				 void **con_cls, enum MHD_RequestTerminationCode toe)
{
	(void)cls; (void)conn; (void)toe;
	if (*con_cls) {
		free(*con_cls);
		*con_cls = NULL;
	}
}

