// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * npu_tap_reader — V3.2.2 NPU tap userspace consumer.
 *
 * Reads the post-effects audio ring buffer written by the SOF DSP firmware
 * via /dev/imx-audio-tap (mmap, pgprot_writecombine).
 * Implements R4 seqcount-style read pattern (re-check epoch around data
 * read, retry if mismatch). Supports R1 epoch detection (DSP reset between
 * `dai_common_params` calls).
 *
 * Modes :
 *   --dump <file.wav>    : write samples to a WAV file (8ch S32_LE 48000)
 *   --stats              : print throughput, drops, epoch transitions
 *   --time <seconds>     : run for N seconds (default = until SIGINT)
 *
 * Build (on board) :
 *   gcc -O2 -Wall -o npu_tap_reader npu_tap_reader.c
 *
 * Layout target (R6 runtime) :
 *   [0x942b0000 .. 0x942b007f]   header (128 B)
 *   [0x942b0080 .. 0x942b0080+ring_size]  ring data
 *   ring_size = (262016 / period_bytes) * period_bytes (V4.2 = 261120)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NPU_TAP_MAGIC          0x5441504EU   /* "NPAT" */
#define NPU_TAP_RING_SIZE      0x40000U      /* 256 KB total */
#define NPU_TAP_HDR_SIZE       128U
#define NPU_TAP_DEV_DEFAULT    "/dev/imx-audio-tap-in"   /* V7.0-E4 default */

struct npu_tap_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t ring_size;
	uint32_t hdr_size;
	uint32_t epoch;
	uint32_t write_idx;
	uint32_t read_idx;
	uint32_t period_bytes;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t frame_fmt;
	uint32_t direction;      /* V7.0-E4 : 0=play (tap-out), 1=cap (tap-in) */
	uint32_t reserved[18];
} __attribute__((packed));

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }

/* WAV header for 8ch S32_LE 48000, length filled at end. */
struct wav_hdr {
	char     riff[4];      uint32_t riff_size;
	char     wave[4];
	char     fmt[4];       uint32_t fmt_size;
	uint16_t fmt_tag;      uint16_t channels;
	uint32_t sample_rate;  uint32_t byte_rate;
	uint16_t block_align;  uint16_t bits_per_sample;
	char     data[4];      uint32_t data_size;
};

static void wav_init(struct wav_hdr *h, uint32_t channels,
		     uint32_t sample_rate, uint16_t bits_per_sample)
{
	memcpy(h->riff, "RIFF", 4);
	memcpy(h->wave, "WAVE", 4);
	memcpy(h->fmt, "fmt ", 4);
	memcpy(h->data, "data", 4);
	h->fmt_size = 16;
	h->fmt_tag = 1; /* PCM */
	h->channels = channels;
	h->sample_rate = sample_rate;
	h->bits_per_sample = bits_per_sample;
	h->block_align = channels * bits_per_sample / 8;
	h->byte_rate = sample_rate * h->block_align;
	h->riff_size = 36;       /* updated at end */
	h->data_size = 0;
}

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Atomic load with acquire semantics on AArch64. */
static inline uint32_t load_acq(volatile const uint32_t *p)
{
	return atomic_load_explicit((const _Atomic uint32_t *)p,
				    memory_order_acquire);
}

/* Compute a - b modulo m (where 0 <= a,b < m). */
static inline uint32_t mod_sub(uint32_t a, uint32_t b, uint32_t m)
{
	return (a >= b) ? (a - b) : (m - (b - a));
}

static int dump_wav_path = 0;
static const char *dump_wav_file = NULL;
static const char *tap_dev = NPU_TAP_DEV_DEFAULT;
static int print_stats = 0;
static double run_seconds = 0.0;   /* 0 = run until SIGINT */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--device path] [--dump file.wav] [--stats] [--time seconds]\n"
		"\n"
		"  --device path    NPU tap device (default %s)\n"
		"  --dump file.wav  Save samples to WAV (8ch S32_LE)\n"
		"  --stats          Print throughput / drops / epoch transitions\n"
		"  --time N         Stop after N seconds (default: until Ctrl+C)\n",
		argv0, NPU_TAP_DEV_DEFAULT);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			tap_dev = argv[++i];
		} else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			dump_wav_file = argv[++i];
			dump_wav_path = 1;
		} else if (!strcmp(argv[i], "--stats")) {
			print_stats = 1;
		} else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
			run_seconds = atof(argv[++i]);
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	int fd = open(tap_dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", tap_dev, strerror(errno));
		return 1;
	}

	uint8_t *region = mmap(NULL, NPU_TAP_RING_SIZE, PROT_READ,
			       MAP_SHARED, fd, 0);
	if (region == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	volatile struct npu_tap_hdr *hdr = (volatile struct npu_tap_hdr *)region;
	uint8_t *data_base = region + NPU_TAP_HDR_SIZE;

	/* Wait for firmware to publish a valid header (magic == NPAT after init).
	 * M5 boot handshake : magic=0 during init, =NPAT at end. */
	int magic_attempts = 0;
	while (g_running) {
		uint32_t m = load_acq(&hdr->magic);
		if (m == NPU_TAP_MAGIC) break;
		if (++magic_attempts == 1)
			fprintf(stderr,
				"npu_tap: waiting for firmware (magic=0x%x, "
				"start aplay or check SOF firmware)\n", m);
		usleep(100000);   /* 100 ms */
	}
	if (!g_running) {
		munmap(region, NPU_TAP_RING_SIZE);
		close(fd);
		return 0;
	}

	uint32_t ring_size = load_acq(&hdr->ring_size);
	uint32_t period_bytes = load_acq(&hdr->period_bytes);
	uint32_t sample_rate = load_acq(&hdr->sample_rate);
	uint32_t channels = load_acq(&hdr->channels);

	fprintf(stderr,
		"npu_tap: header valid — version=%u ring_size=%u period=%u "
		"rate=%u ch=%u\n",
		hdr->version, ring_size, period_bytes, sample_rate, channels);

	FILE *wav = NULL;
	struct wav_hdr wh;
	if (dump_wav_path) {
		wav = fopen(dump_wav_file, "wb");
		if (!wav) {
			fprintf(stderr, "fopen(%s): %s\n",
				dump_wav_file, strerror(errno));
			munmap(region, NPU_TAP_RING_SIZE);
			close(fd);
			return 1;
		}
		wav_init(&wh, channels, sample_rate, 32);
		fwrite(&wh, sizeof(wh), 1, wav);
	}

	uint8_t *bounce = malloc(ring_size);
	if (!bounce) {
		fprintf(stderr, "malloc bounce: %s\n", strerror(errno));
		if (wav) fclose(wav);
		munmap(region, NPU_TAP_RING_SIZE);
		close(fd);
		return 1;
	}

	uint32_t local_read = load_acq(&hdr->write_idx);
	uint32_t last_epoch = load_acq(&hdr->epoch);
	uint64_t bytes_consumed = 0;
	uint64_t epoch_resets = 0;
	uint64_t race_retries = 0;
	uint64_t wav_bytes = 0;

	double t_start = now_s();
	double t_last_stat = t_start;
	uint64_t bytes_at_last_stat = 0;

	fprintf(stderr, "npu_tap: starting (epoch=%u read_idx=%u)\n",
		last_epoch, local_read);

	while (g_running) {
		double t_now = now_s();
		if (run_seconds > 0.0 && t_now - t_start >= run_seconds)
			break;

		/* R4 seqcount-style read */
		uint32_t e1 = load_acq(&hdr->epoch);
		if (e1 != last_epoch) {
			/* DSP did params reset — discard local state */
			local_read = load_acq(&hdr->write_idx);
			last_epoch = e1;
			epoch_resets++;
			fprintf(stderr,
				"npu_tap: DSP reset detected, epoch -> %u "
				"(read_idx resync = %u)\n", e1, local_read);
			continue;
		}

		uint32_t w = load_acq(&hdr->write_idx);
		if (w == local_read) {
			usleep(2000);   /* 2 ms ≈ 1 period */
			continue;
		}

		uint32_t avail = mod_sub(w, local_read, ring_size);
		uint32_t to_end = ring_size - local_read;
		if (avail <= to_end) {
			memcpy(bounce, data_base + local_read, avail);
		} else {
			memcpy(bounce, data_base + local_read, to_end);
			memcpy(bounce + to_end, data_base, avail - to_end);
		}

		uint32_t e2 = load_acq(&hdr->epoch);
		if (e2 != e1) {
			race_retries++;
			fprintf(stderr,
				"npu_tap: race retry (e1=%u e2=%u), "
				"discarding %u bytes\n", e1, e2, avail);
			local_read = load_acq(&hdr->write_idx);
			last_epoch = e2;
			continue;
		}

		/* Data committed (e1 == e2) — consume */
		if (wav) {
			fwrite(bounce, 1, avail, wav);
			wav_bytes += avail;
		}
		/* TODO J5 : push to NPU. For now, just count. */

		local_read = w;
		bytes_consumed += avail;

		if (print_stats && t_now - t_last_stat >= 1.0) {
			uint64_t delta = bytes_consumed - bytes_at_last_stat;
			double mbps = (double)delta / (t_now - t_last_stat) / 1e6;
			fprintf(stderr,
				"npu_tap: %.2f MB/s (epoch=%u resets=%lu "
				"races=%lu total=%lu B)\n",
				mbps, last_epoch,
				(unsigned long)epoch_resets,
				(unsigned long)race_retries,
				(unsigned long)bytes_consumed);
			bytes_at_last_stat = bytes_consumed;
			t_last_stat = t_now;
		}
	}

	double t_end = now_s();
	double avg_mbps = (double)bytes_consumed / (t_end - t_start) / 1e6;

	if (wav) {
		/* Patch WAV sizes */
		wh.data_size = (uint32_t)wav_bytes;
		wh.riff_size = (uint32_t)(36 + wav_bytes);
		fseek(wav, 0, SEEK_SET);
		fwrite(&wh, sizeof(wh), 1, wav);
		fclose(wav);
		fprintf(stderr,
			"npu_tap: wrote %lu bytes to %s (%.2f s, %.2f MB/s)\n",
			(unsigned long)wav_bytes, dump_wav_file,
			t_end - t_start, avg_mbps);
	}
	fprintf(stderr,
		"npu_tap: done — total=%lu B  duration=%.2f s  avg=%.2f MB/s  "
		"epoch_resets=%lu  race_retries=%lu\n",
		(unsigned long)bytes_consumed, t_end - t_start, avg_mbps,
		(unsigned long)epoch_resets, (unsigned long)race_retries);

	free(bounce);
	munmap(region, NPU_TAP_RING_SIZE);
	close(fd);
	return 0;
}
