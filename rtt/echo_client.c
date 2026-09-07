/*
 * echo_client.c -- CIS 7000 warm-up, Parts 1 and 2.
 *
 * Sends fixed-size requests and records the round-trip time for each response.
 * The output includes every measured sample.
 *
 *   ./echo_client <server IP> [--port N] [--size B] [--samples N] [--warmup N]
 *                 [--spin] [--pace-us U] [--out FILE] [--label NAME]
 *
 * Protocol: a 4-byte big-endian length, then that many bytes; the server echoes
 * both back. Header and body go out in a single write().
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <time.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define MAX_MSG 65536

static void die(const char *what) { perror(what); exit(1); }

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	/* clock_gettime uses the vDSO on this hardware and costs about 25 ns. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_exact(int fd, void *buf, size_t n, int spin)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
		if (r > 0) { got += (size_t)r; continue; }
		if (r == 0) return 0;
		if (errno == EINTR) continue;
		if (spin && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		return -1;
	}
	return 1;
}

static int write_all(int fd, const void *buf, size_t n, int spin)
{
	size_t sent = 0;
	while (sent < n) {
		ssize_t w = send(fd, (const char *)buf + sent, n - sent, 0);
		if (w > 0) { sent += (size_t)w; continue; }
		if (errno == EINTR) continue;
		if (spin && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		return -1;
	}
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

/* CPU seconds (user + system) this process has used so far. Reported over the
 * measurement window so each mode's CPU cost sits next to its latency. */
static double cpu_seconds(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	       (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	const char *out_path = NULL, *label = "tcp";
	int port = 9000, size = 64, spin = 0;
	long samples = 100000, warmup = 10000, pace_us = 0;

	static struct option opts[] = {
		{"port",     required_argument, 0, 'p'},
		{"size",     required_argument, 0, 'z'},
		{"samples",  required_argument, 0, 'N'},
		{"warmup",   required_argument, 0, 'W'},
		{"spin",     no_argument,       0, 's'},
		{"pace-us",  required_argument, 0, 'P'},
		{"out",      required_argument, 0, 'o'},
		{"label",    required_argument, 0, 'l'},
		{0, 0, 0, 0}
	};
	int c;
	while ((c = getopt_long(argc, argv, "p:z:N:W:sP:o:l:", opts, NULL)) != -1) {
		switch (c) {
		case 'p': port = atoi(optarg); break;
		case 'z': size = atoi(optarg); break;
		case 'N': samples = atol(optarg); break;
		case 'W': warmup = atol(optarg); break;
		case 's': spin = 1; break;
		case 'P': pace_us = atol(optarg); break;
		case 'o': out_path = optarg; break;
		case 'l': label = optarg; break;
		default: return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s <server IP> [options]\n", argv[0]);
		return 1;
	}
	const char *srv = argv[optind];
	if (size < 1 || size > MAX_MSG) { fprintf(stderr, "bad --size\n"); return 1; }

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) die("socket");

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, srv, &sa.sin_addr) != 1) {
		fprintf(stderr, "bad server IP %s\n", srv); return 1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) die("connect");

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (spin) {
		int fl = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	}

	/* One buffer holds header and body so each request is a single write(). */
	char *req = malloc((size_t)size + 4), *rsp = malloc(MAX_MSG);
	/* Preallocate: never write a sample inside the measurement loop. */
	uint64_t *lat = malloc(sizeof(uint64_t) * (size_t)samples);
	uint64_t *at  = malloc(sizeof(uint64_t) * (size_t)samples);
	if (!req || !rsp || !lat || !at) die("malloc");

	uint32_t nlen = htonl((uint32_t)size);
	memcpy(req, &nlen, 4);
	memset(req + 4, 'x', (size_t)size);

	/* Open the output file first, so a path that cannot be written fails
	 * here and no run is wasted. */
	FILE *out = NULL;
	if (out_path && !(out = fopen(out_path, "w"))) die("fopen --out");

	/* Identify the run in the output so a CSV can always be traced back to
	 * what actually ran. Keep this with your raw data. */
	{
		char ts[64]; time_t t = time(NULL);
		strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
		printf("benchmark: tcp_echo\nrole: client\nmode: %s\nserver: %s:%d\n"
		       "payload_bytes: %d\nsamples: %ld\nwarmup: %ld\nlabel: %s\ntimestamp: %s\n",
		       spin ? "spin" : "blocking", srv, port, size, samples, warmup, label, ts);
		fflush(stdout);
	}

	long total = warmup + samples, n = 0;
	double cpu0 = cpu_seconds();
	uint64_t t_start = now_ns();

	for (long i = 0; i < total; i++) {
		uint64_t t0 = now_ns();

		if (write_all(fd, req, (size_t)size + 4, spin) < 0) die("send");

		uint32_t rlen_n;
		if (read_exact(fd, &rlen_n, sizeof(rlen_n), spin) <= 0) {
			fprintf(stderr, "server closed after %ld requests\n", i); break;
		}
		uint32_t rlen = ntohl(rlen_n);
		if (rlen > MAX_MSG) { fprintf(stderr, "bad reply length\n"); break; }
		if (read_exact(fd, rsp, rlen, spin) <= 0) {
			fprintf(stderr, "short reply after %ld requests\n", i); break;
		}

		uint64_t t1 = now_ns();
		if (i >= warmup && n < samples) {
			lat[n] = t1 - t0;
			at[n]  = t0 - t_start;   /* wall offset, to plot latency vs time */
			n++;
		}

		if (pace_us > 0) {
			/* Spread the run over wall-clock time so slow-timescale tail
			 * sources (C-state exits, timer ticks, RCU) are sampled. 100k
			 * back-to-back samples finish in a few seconds, too short to see them. */
			struct timespec ts = { .tv_sec = 0, .tv_nsec = pace_us * 1000 };
			nanosleep(&ts, NULL);
		}
	}

	double cpu_used = cpu_seconds() - cpu0;
	double wall = (now_ns() - t_start) / 1e9;

	close(fd);
	if (n == 0) { fprintf(stderr, "no samples collected\n"); return 1; }

	if (out) {
		/* Raw samples only; compute percentiles yourself. t_offset_us is the
		 * sample's start time from the beginning of the run, for plotting
		 * latency against time. Run metadata is on stdout. */
		fprintf(out, "sample,t_offset_us,t_us\n");
		for (long i = 0; i < n; i++)
			fprintf(out, "%ld,%.3f,%.3f\n", i, at[i] / 1000.0, lat[i] / 1000.0);
		fclose(out);
		fprintf(stderr, "wrote %ld samples to %s\n", n, out_path);
	}

	double sum = 0;
	for (long i = 0; i < n; i++) sum += lat[i];
	double mean_us = sum / n / 1000.0;

	qsort(lat, (size_t)n, sizeof(uint64_t), cmp_u64);
	printf("%s n=%ld size=%d spin=%d | "
	       "min %.2f p50 %.2f mean %.2f p99 %.2f p99.9 %.2f max %.2f us\n",
	       label, n, size, spin,
	       lat[0] / 1000.0,
	       lat[n / 2] / 1000.0,
	       mean_us,
	       lat[(long)(n * 0.99)] / 1000.0,
	       lat[(long)(n * 0.999)] / 1000.0,
	       lat[n - 1] / 1000.0);

	/* CPU used by the client. A blocking client sleeps between replies and
	 * uses a fraction of a core; a spinning client holds a whole core for the
	 * run. */
	printf("%s cpu: %.3f s over %.3f s wall = %.1f%% of one core "
	       "(%.2f us of CPU per request)\n",
	       label, cpu_used, wall, wall > 0 ? 100.0 * cpu_used / wall : 0.0,
	       n > 0 ? 1e6 * cpu_used / (double)(warmup + n) : 0.0);

	free(req); free(rsp); free(lat); free(at);
	return 0;
}
