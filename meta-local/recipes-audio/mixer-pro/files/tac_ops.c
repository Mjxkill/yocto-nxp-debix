// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tac_ops — ops control du domaine TAC5212/ALSA : contrôles amixer (cards
 * TAC) + lecture de registres via i2c-dev. RAPPEL règle produit : les
 * effets TAC sont STATIQUES — jamais écrits pendant le live (cause racine
 * des plops 2026-07, anti-larsen v1). Corps déplacés tels quels depuis
 * handle_cmd (V14.0 étape 4b, extraction pure). Proto : control.h.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#include "util.h"        /* mlog */
#include "control.h"

/* V14.0 étape 4b : ops control du domaine TAC/ALSA (amixer + registres
 * i2c en lecture — effets TAC STATIQUES, jamais écrits en live).
 * Corps déplacés tels quels depuis handle_cmd (extraction pure). */
int tac_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "set_alsa")) {
		/* V9.4.1 — Set ALSA control (INTEGER seul pour V9.4.1).
		 * Format :
		 *   {"op":"set_alsa","name":"PGA2.0 2 Out Strip1 Volume","value":50}
		 *   {"op":"set_alsa","numid":324,"value":50}
		 *
		 * Hardcode card "softac5212tdm" (le seul DSP HiFi4 SOF expose les
		 * kcontrols MULTIBAND_DRC + PGA + TAC BQ). Pour BYTES blob (DRC
		 * raw), implementation en V9.4.2 (besoin parser hex/base64). */
		char ctrl_name[128] = {0};
		int numid = 0;
		float value = 0;
		int by_numid = (json_get_int(line, "numid", &numid) == 0);
		int by_name  = (json_get_str(line, "name", ctrl_name, sizeof(ctrl_name)) == 0);
		/* V9.4.3 : "value" optionnel — pas requis pour BYTES (qui prend "bytes"). */
		int has_value = (json_get_float(line, "value", &value) == 0);
		if (!by_numid && !by_name) {
			dprintf(fd, "{\"ok\":false,\"err\":\"need numid or name\"}\n");
			return 1;
		}
		snd_ctl_t *h = NULL;
		if (snd_ctl_open(&h, "hw:CARD=softac5212tdm", 0) < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_open failed\"}\n");
			return 1;
		}
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_id_alloca(&id);
		if (by_numid) snd_ctl_elem_id_set_numid(id, numid);
		else {
			snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
			snd_ctl_elem_id_set_name(id, ctrl_name);
		}
		snd_ctl_elem_info_t *info;
		snd_ctl_elem_info_alloca(&info);
		snd_ctl_elem_info_set_id(info, id);
		int rc = snd_ctl_elem_info(h, info);
		if (rc < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"control not found\"}\n");
			return 1;
		}
		snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
		snd_ctl_elem_value_t *val;
		snd_ctl_elem_value_alloca(&val);
		snd_ctl_elem_value_set_id(val, id);
		if (type == SND_CTL_ELEM_TYPE_INTEGER) {
			if (!has_value) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"INTEGER needs value\"}\n");
				return 1;
			}
			unsigned int n_chan = snd_ctl_elem_info_get_count(info);
			for (unsigned int c = 0; c < n_chan; c++)
				snd_ctl_elem_value_set_integer(val, c, (long)value);
		} else if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
			if (!has_value) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"BOOLEAN needs value\"}\n");
				return 1;
			}
			snd_ctl_elem_value_set_boolean(val, 0, value != 0.0f);
		} else if (type == SND_CTL_ELEM_TYPE_BYTES) {
			/* V9.4.3 : parse "bytes":"<hex>" → raw bytes → snd_ctl set.
			 * Pour DRC blob 4096 octets = 8192 chars hex requis.
			 * Le control count = nb max d'octets attendu. */
			const char *hex_p = strstr(line, "\"bytes\"");
			if (!hex_p) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"BYTES needs hex field\"}\n");
				return 1;
			}
			hex_p = strchr(hex_p, '"');     /* skip "bytes" */
			if (hex_p) hex_p = strchr(hex_p + 1, '"');   /* skip : */
			if (hex_p) hex_p = strchr(hex_p + 1, '"');   /* opening " of value */
			if (!hex_p) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"bad bytes format\"}\n");
				return 1;
			}
			hex_p++;
			const char *hex_end = strchr(hex_p, '"');
			if (!hex_end) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"unclosed bytes\"}\n");
				return 1;
			}
			size_t hex_len = (size_t)(hex_end - hex_p);
			if (hex_len % 2 != 0) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"odd hex length\"}\n");
				return 1;
			}
			size_t n_bytes = hex_len / 2;
			unsigned int max_bytes = snd_ctl_elem_info_get_count(info);
			if (n_bytes > max_bytes) {
				snd_ctl_close(h);
				dprintf(fd, "{\"ok\":false,\"err\":\"too many bytes (%zu > %u)\"}\n",
				        n_bytes, max_bytes);
				return 1;
			}
			/* Parse hex into raw bytes — local stack buf 4 KB suffit pour DRC */
			static unsigned char raw[4096];
			for (size_t i = 0; i < n_bytes && i < sizeof(raw); i++) {
				char c1 = hex_p[i*2], c2 = hex_p[i*2 + 1];
				int hi = (c1 <= '9') ? c1 - '0' : ((c1 | 0x20) - 'a' + 10);
				int lo = (c2 <= '9') ? c2 - '0' : ((c2 | 0x20) - 'a' + 10);
				if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
					snd_ctl_close(h);
					dprintf(fd, "{\"ok\":false,\"err\":\"bad hex char\"}\n");
					return 1;
				}
				raw[i] = (unsigned char)((hi << 4) | lo);
			}
			for (size_t i = 0; i < n_bytes; i++)
				snd_ctl_elem_value_set_byte(val, (unsigned int)i, raw[i]);
		} else {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"unsupported control type\"}\n");
			return 1;
		}
		rc = snd_ctl_elem_write(h, val);
		snd_ctl_close(h);
		if (rc < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_elem_write failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"op\":\"set_alsa\",\"value\":%.4f}\n", value);
		}
		return 1;
	}
	if (json_has_op(line, "get_alsa")) {
		/* Format : {"op":"get_alsa","name":"..."} ou numid */
		char ctrl_name[128] = {0};
		int numid = 0;
		int by_numid = (json_get_int(line, "numid", &numid) == 0);
		int by_name  = (json_get_str(line, "name", ctrl_name, sizeof(ctrl_name)) == 0);
		if (!by_numid && !by_name) { dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n"); return 1; }
		snd_ctl_t *h = NULL;
		if (snd_ctl_open(&h, "hw:CARD=softac5212tdm", 0) < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"snd_ctl_open failed\"}\n"); return 1;
		}
		snd_ctl_elem_id_t *id;
		snd_ctl_elem_id_alloca(&id);
		if (by_numid) snd_ctl_elem_id_set_numid(id, numid);
		else { snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		       snd_ctl_elem_id_set_name(id, ctrl_name); }
		/* V9.4.3 : type-aware read. Lookup type via info pour distinguer
		 * INTEGER (value:N) de BYTES (bytes:"hex"). */
		snd_ctl_elem_info_t *ginfo;
		snd_ctl_elem_info_alloca(&ginfo);
		snd_ctl_elem_info_set_id(ginfo, id);
		if (snd_ctl_elem_info(h, ginfo) < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"info failed\"}\n"); return 1;
		}
		snd_ctl_elem_type_t gtype = snd_ctl_elem_info_get_type(ginfo);
		snd_ctl_elem_value_t *val;
		snd_ctl_elem_value_alloca(&val);
		snd_ctl_elem_value_set_id(val, id);
		if (snd_ctl_elem_read(h, val) < 0) {
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":false,\"err\":\"read failed\"}\n"); return 1;
		}
		if (gtype == SND_CTL_ELEM_TYPE_BYTES) {
			unsigned int n_bytes = snd_ctl_elem_info_get_count(ginfo);
			if (n_bytes > 4096) n_bytes = 4096;   /* cap pour hex 8 KB output */
			static char hex_out[8200];
			for (unsigned int i = 0; i < n_bytes; i++) {
				unsigned char b = snd_ctl_elem_value_get_byte(val, i);
				static const char hex_chars[] = "0123456789abcdef";
				hex_out[i*2]     = hex_chars[(b >> 4) & 0xF];
				hex_out[i*2 + 1] = hex_chars[b & 0xF];
			}
			hex_out[n_bytes * 2] = '\0';
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":true,\"bytes\":\"%s\",\"len\":%u}\n", hex_out, n_bytes);
		} else {
			long v = snd_ctl_elem_value_get_integer(val, 0);
			snd_ctl_close(h);
			dprintf(fd, "{\"ok\":true,\"value\":%ld}\n", v);
		}
		return 1;
	}
	if (json_has_op(line, "set_tac_reg")) {
		/* V9.4.2 — Set TAC5212 codec register via i2c-3.
		 * Format : {"op":"set_tac_reg","tac":0..3,"reg":0xRR,"value":0xVV}
		 *
		 * 4 codecs TAC5212 aux addresses 0x50, 0x51, 0x52, 0x53 sur /dev/i2c-3.
		 * Le codec utilise des pages registres (reg 0x00 = page select).
		 * NPU peut writer reg 0x00 séparément pour switcher page.
		 *
		 * I2C_SLAVE_FORCE car le driver tac5212 kernel tient le device.
		 * Risque : désync driver/hw si on touche les registres init driver.
		 * En pratique le NPU vise les registres DRC/limiter/BQ que le driver
		 * ne reset jamais après init. */
		int tac_idx = 0, reg = 0;
		float val_f = 0;
		if (json_get_int(line, "tac", &tac_idx) < 0 ||
		    json_get_int(line, "reg", &reg) < 0 ||
		    json_get_float(line, "value", &val_f) < 0 ||
		    tac_idx < 0 || tac_idx > 3 ||
		    reg < 0 || reg > 0xFF) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args (tac 0..3, reg 0..255)\"}\n");
			return 1;
		}
		int fd_i2c = open("/dev/i2c-3", O_RDWR);
		if (fd_i2c < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"open i2c-3 failed: %s\"}\n", strerror(errno));
			return 1;
		}
		if (ioctl(fd_i2c, 0x0706 /*I2C_SLAVE_FORCE*/, 0x50 + tac_idx) < 0) {
			close(fd_i2c);
			dprintf(fd, "{\"ok\":false,\"err\":\"ioctl I2C_SLAVE_FORCE failed: %s\"}\n", strerror(errno));
			return 1;
		}
		uint8_t buf[2] = { (uint8_t)reg, (uint8_t)(int)val_f };
		ssize_t w = write(fd_i2c, buf, 2);
		close(fd_i2c);
		if (w != 2) {
			dprintf(fd, "{\"ok\":false,\"err\":\"i2c write failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"op\":\"set_tac_reg\",\"tac\":%d,"
			            "\"reg\":%d,\"value\":%d}\n",
			        tac_idx, reg, (int)val_f);
		}
		return 1;
	}
	if (json_has_op(line, "get_tac_reg")) {
		/* Format : {"op":"get_tac_reg","tac":0..3,"reg":0xRR}
		 * → {"ok":true,"value":N} */
		int tac_idx = 0, reg = 0;
		if (json_get_int(line, "tac", &tac_idx) < 0 ||
		    json_get_int(line, "reg", &reg) < 0 ||
		    tac_idx < 0 || tac_idx > 3 || reg < 0 || reg > 0xFF) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n");
			return 1;
		}
		int fd_i2c = open("/dev/i2c-3", O_RDWR);
		if (fd_i2c < 0) {
			dprintf(fd, "{\"ok\":false,\"err\":\"open failed\"}\n"); return 1;
		}
		if (ioctl(fd_i2c, 0x0706 /*I2C_SLAVE_FORCE*/, 0x50 + tac_idx) < 0) {
			close(fd_i2c);
			dprintf(fd, "{\"ok\":false,\"err\":\"ioctl failed\"}\n"); return 1;
		}
		uint8_t r = (uint8_t)reg, v = 0;
		ssize_t ww = write(fd_i2c, &r, 1);
		ssize_t rr = read(fd_i2c, &v, 1);
		close(fd_i2c);
		if (ww != 1 || rr != 1) {
			dprintf(fd, "{\"ok\":false,\"err\":\"i2c read failed\"}\n");
		} else {
			dprintf(fd, "{\"ok\":true,\"value\":%d}\n", v);
		}
		return 1;
	}
	return 0;
}
