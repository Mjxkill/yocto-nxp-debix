// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * V7.0-E6.d — mixerctl : client CLI pour mixer-pro daemon.
 *
 * Envoie une commande JSON sur /run/mixer-pro.sock et affiche la réponse.
 *
 * Usage :
 *   mixerctl send <in> <bus> <gain>          ; in=0..25 bus=0..7
 *   mixerctl master <src> <out> <gain>       ; src=0..33 out=0..17
 *   mixerctl fx <bus> <gain>                 ; bus=0..7
 *   mixerctl mute <src> {0|1}
 *   mixerctl state
 *   mixerctl reset
 *   mixerctl raw '{"op":...}'
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCK_PATH "/run/mixer-pro.sock"

static int send_json(const char *json)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(fd);
		return 1;
	}

	char line[512];
	int n = snprintf(line, sizeof(line), "%s\n", json);
	if (write(fd, line, n) != n) { perror("write"); close(fd); return 1; }

	char buf[1024];
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	if (r > 0) { buf[r] = 0; fputs(buf, stdout); }

	close(fd);
	return 0;
}

static void usage(void)
{
	fputs("usage:\n"
	      "  mixerctl send   <in> <bus> <gain>\n"
	      "  mixerctl master <src> <out> <gain>\n"
	      "  mixerctl fx     <bus> <gain>\n"
	      "  mixerctl mute   <src> {0|1}\n"
	      "  mixerctl state\n"
	      "  mixerctl reset\n"
	      "  mixerctl raw    '{\"op\":...}'\n",
	      stderr);
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }

	char json[512];

	if (!strcmp(argv[1], "send") && argc == 5) {
		snprintf(json, sizeof(json),
			 "{\"op\":\"set_send\",\"in\":%s,\"bus\":%s,\"gain\":%s}",
			 argv[2], argv[3], argv[4]);
		return send_json(json);
	}
	if (!strcmp(argv[1], "master") && argc == 5) {
		snprintf(json, sizeof(json),
			 "{\"op\":\"set_master\",\"src\":%s,\"out\":%s,\"gain\":%s}",
			 argv[2], argv[3], argv[4]);
		return send_json(json);
	}
	if (!strcmp(argv[1], "fx") && argc == 4) {
		snprintf(json, sizeof(json),
			 "{\"op\":\"set_fx_bus\",\"bus\":%s,\"gain\":%s}",
			 argv[2], argv[3]);
		return send_json(json);
	}
	if (!strcmp(argv[1], "mute") && argc == 4) {
		snprintf(json, sizeof(json),
			 "{\"op\":\"set_mute\",\"src\":%s,\"mute\":%s}",
			 argv[2], argv[3]);
		return send_json(json);
	}
	if (!strcmp(argv[1], "state") && argc == 2) {
		return send_json("{\"op\":\"get_state\"}");
	}
	if (!strcmp(argv[1], "reset") && argc == 2) {
		return send_json("{\"op\":\"reset\"}");
	}
	if (!strcmp(argv[1], "raw") && argc == 3) {
		return send_json(argv[2]);
	}

	usage();
	return 2;
}
