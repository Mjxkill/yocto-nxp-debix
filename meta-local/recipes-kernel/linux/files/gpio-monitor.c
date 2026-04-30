/*
 * gpio-monitor.c — Precise FSYNC frequency measurement via GPIO interrupt
 * Measures time between FSYNC rising edges using clock_gettime(CLOCK_MONOTONIC)
 *
 * GPIO5_IO06 = 134 = ECSPI1_SCLK = RX_SYNC (reflects TX_FSYNC via PCB)
 *
 * Usage: gpio-monitor <num_edges>
 * Default: 1000 edges
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

static int gpio_setup_irq(int gpio_num, const char *edge)
{
	char path[64];
	int fd;
	char buf[8];
	int n;

	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd >= 0) {
		n = snprintf(buf, sizeof(buf), "%d", gpio_num);
		write(fd, buf, n);
		close(fd);
	}

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio_num);
	fd = open(path, O_WRONLY);
	if (fd >= 0) { write(fd, "in", 2); close(fd); }

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", gpio_num);
	fd = open(path, O_WRONLY);
	if (fd >= 0) { write(fd, edge, strlen(edge)); close(fd); }

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio_num);
	return open(path, O_RDONLY);
}

static void gpio_consume(int fd)
{
	char c;
	lseek(fd, 0, SEEK_SET);
	read(fd, &c, 1);
}

static long long timespec_to_ns(struct timespec *ts)
{
	return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

int main(int argc, char *argv[])
{
	int fd_fsync;
	struct pollfd pfd;
	int num_edges = 1000;
	int count = 0;
	struct timespec first, last, now;
	long long elapsed_ns;
	double freq;
	long long *intervals = NULL;
	long long prev_ns, cur_ns;
	long long min_interval = 0x7FFFFFFFFFFFFFFFLL;
	long long max_interval = 0;
	long long total_interval = 0;

	if (argc > 1)
		num_edges = atoi(argv[1]);

	intervals = malloc(num_edges * sizeof(long long));

	fd_fsync = gpio_setup_irq(134, "rising");
	if (fd_fsync < 0) {
		fprintf(stderr, "Cannot open GPIO 134\n");
		return 1;
	}

	gpio_consume(fd_fsync);
	pfd.fd = fd_fsync;
	pfd.events = POLLPRI | POLLERR;

	printf("Waiting for %d FSYNC rising edges...\n", num_edges);

	/* Wait for first edge */
	while (1) {
		if (poll(&pfd, 1, 5000) > 0 && (pfd.revents & POLLPRI)) {
			gpio_consume(fd_fsync);
			clock_gettime(CLOCK_MONOTONIC, &first);
			prev_ns = timespec_to_ns(&first);
			count = 1;
			break;
		} else {
			printf("No FSYNC detected (timeout 5s)\n");
			close(fd_fsync);
			free(intervals);
			return 1;
		}
	}

	/* Count remaining edges */
	while (count < num_edges) {
		if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLPRI)) {
			gpio_consume(fd_fsync);
			clock_gettime(CLOCK_MONOTONIC, &now);
			cur_ns = timespec_to_ns(&now);

			long long interval = cur_ns - prev_ns;
			intervals[count - 1] = interval;
			if (interval < min_interval) min_interval = interval;
			if (interval > max_interval) max_interval = interval;
			total_interval += interval;

			prev_ns = cur_ns;
			count++;
		} else {
			printf("FSYNC stopped after %d edges\n", count);
			break;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &last);
	elapsed_ns = timespec_to_ns(&last) - timespec_to_ns(&first);

	close(fd_fsync);

	printf("Edges captured: %d\n", count);
	printf("Total time: %.3f ms\n", (double)elapsed_ns / 1000000.0);

	if (count > 1) {
		freq = (double)(count - 1) / ((double)elapsed_ns / 1000000000.0);
		printf("FSYNC frequency: %.2f Hz\n", freq);
		printf("FSYNC period: avg=%.2f us  min=%.2f us  max=%.2f us\n",
		       (double)total_interval / (count - 1) / 1000.0,
		       (double)min_interval / 1000.0,
		       (double)max_interval / 1000.0);
		printf("Expected: 48000 Hz = 20.83 us period\n");
	}

	free(intervals);
	return 0;
}
