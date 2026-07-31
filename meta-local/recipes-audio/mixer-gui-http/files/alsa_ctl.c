// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * alsa_ctl — contrôles TAC/ALSA : amixer (whitelist), tac-reset,
 * blobs TLV SOF (miroir DSP_BLOB_DIR), alsactl store débouncé.
 * Déplacé tel quel (V14.0 étape 6).
 */
#include "gui_http.h"

/* V10-N7b : rémanence TAC/PGA — alsactl store débouncé (3 s) après
 * chaque /api/alsa/set (le ExecStop d'alsa-restore ne couvre que les
 * shutdowns propres, pas les coupures secteur) */
_Atomic int g_alsa_dirty = 0;
void *alsa_store_thread(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(3);
		if (atomic_exchange(&g_alsa_dirty, 0))
			(void)system("alsactl store >/dev/null 2>&1");
	}
	return NULL;
}

/* ============================== ALSA amixer helpers (E7.3b) ======= */

#define ALSA_CARD "softac5212tdm"

/* Validate value string : whitelist [0-9a-zA-Z .,_-]. Avoid shell injection
 * even if we use fork+exec without shell — defense in depth. Limit 256 chars
 * to accommodate BYTES blobs (e.g. TAC5212 biquad coefs : 20 decimal bytes
 * comma-separated ≈ 80 chars). */
int amixer_value_safe(const char *v)
{
	if (!v || !*v) return 0;
	for (const char *p = v; *p; p++) {
		char c = *p;
		/* '/' requis par les items d'enum TAC ("3 Biquads/Ch") —
		 * sans lui le forçage biquads de l'EQ échouait en silence */
		if (!(isalnum((unsigned char)c) || c == ' ' || c == '.' ||
		      c == ',' || c == '-' || c == '_' || c == '/'))
			return 0;
	}
	return strlen(v) < 256;
}

/* Run `amixer -c softac5212tdm contents` and capture stdout into out[cap].
 * Returns bytes read on success, -1 on error. */
int run_amixer_contents(char *out, size_t cap)
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
int run_amixer_cset(int numid, const char *value)
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

int run_tac_reset(const char *mode, int *exitp)
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
int sof_blob_read(int numid, unsigned char *out, size_t cap)
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
int sof_blob_write(int numid, const unsigned char *data, size_t size)
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
void hex_encode(const unsigned char *src, size_t size, char *dst)
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
int hex_decode(const char *src, unsigned char *dst, size_t cap)
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
int json_get_int_field(const char *s, const char *key, int *out)
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
int json_get_str_field(const char *s, const char *key, char *out, size_t cap)
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

