/*
 * echo_server.c -- CIS 7000 warm-up, Parts 1 and 2.
 *
 * TCP echo server. Modes:
 *   default   blocking read()/write(); the thread sleeps while waiting
 *   --spin    non-blocking recv() polling
 *
 * Handles one connection at a time for a single request-response path.
 *
 *   ./echo_server [--bind IP] [--port N] [--spin] [--quiet]
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
#include <signal.h>
#include <time.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define MAX_MSG 65536

static volatile sig_atomic_t stop;
static void on_sigint(int s) { (void)s; stop = 1; }

static void die(const char *what) { perror(what); exit(1); }

/* CPU and wall-clock time used for the per-connection utilization report. */
static double cpu_seconds(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	       (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static double wall_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Read exactly n bytes. In spin mode a would-block means the data has not
 * landed yet; keep polling. */
static int read_exact(int fd, void *buf, size_t n, int spin)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
		if (r > 0) { got += (size_t)r; continue; }
		if (r == 0) return 0;                       /* peer closed */
		if (errno == EINTR) continue;
		if (spin && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (stop) return 0;
			continue;
		}
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

int main(int argc, char **argv)
{
	const char *bind_ip = "0.0.0.0";
	int port = 9000, spin = 0, quiet = 0;

	static struct option opts[] = {
		{"bind",   required_argument, 0, 'b'},
		{"port",   required_argument, 0, 'p'},
		{"spin",   no_argument,       0, 's'},
		{"quiet",  no_argument,       0, 'q'},
		{0, 0, 0, 0}
	};
	int c;
	while ((c = getopt_long(argc, argv, "b:p:sq", opts, NULL)) != -1) {
		switch (c) {
		case 'b': bind_ip = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 's': spin = 1; break;
		case 'q': quiet = 1; break;
		default:
			fprintf(stderr, "usage: %s [--bind IP] [--port N] [--spin] [--quiet]\n",
				argv[0]);
			return 1;
		}
	}

	/* sigaction without SA_RESTART: glibc's signal() restarts a blocked
	 * accept()/read() after the handler runs, so Ctrl-C would set the flag
	 * and the server would sit in the syscall until the next connection. */
	struct sigaction sa_int = { .sa_handler = on_sigint };
	sigemptyset(&sa_int.sa_mask);
	sigaction(SIGINT, &sa_int, NULL);
	sigaction(SIGTERM, &sa_int, NULL);
	signal(SIGPIPE, SIG_IGN);

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) die("socket");
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_ip, &sa.sin_addr) != 1) {
		fprintf(stderr, "bad --bind address %s\n", bind_ip);
		return 1;
	}
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) die("bind");
	if (listen(ls, 16) < 0) die("listen");

	if (!quiet) {
		char ts[64]; time_t t = time(NULL);
		strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
		printf("benchmark: tcp_echo\nrole: server\nmode: %s\nbind: %s:%d\ntimestamp: %s\n",
		       spin ? "spin" : "blocking", bind_ip, port, ts);
	}
	fflush(stdout);

	char *buf = malloc(MAX_MSG);
	if (!buf) die("malloc");

	while (!stop) {
		int fd = accept(ls, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR) continue;
			die("accept");
		}

		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (spin) {
			int fl = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		}

		if (!quiet) { printf("client connected\n"); fflush(stdout); }
		double cpu0 = cpu_seconds(), t0 = wall_seconds();

		/* Protocol: 4-byte big-endian length, then that many bytes.
		 * Echo the payload back unchanged, with the same framing. */
		for (;;) {
			uint32_t nlen;
			int r = read_exact(fd, &nlen, sizeof(nlen), spin);
			if (r <= 0) break;
			uint32_t len = ntohl(nlen);
			if (len == 0 || len > MAX_MSG) break;

			if (read_exact(fd, buf, len, spin) <= 0) break;

			if (write_all(fd, &nlen, sizeof(nlen), spin) < 0) break;
			if (write_all(fd, buf, len, spin) < 0) break;
		}

		close(fd);
		{
			double cpu_used = cpu_seconds() - cpu0, wall = wall_seconds() - t0;
			printf("server cpu: %.3f s over %.3f s wall = %.1f%% of one core "
			       "(mode=%s)\n", cpu_used, wall,
			       wall > 0 ? 100.0 * cpu_used / wall : 0.0,
			       spin ? "spin" : "blocking");
			fflush(stdout);
		}
		if (!quiet) { printf("client disconnected\n"); fflush(stdout); }
	}

	free(buf);
	close(ls);
	return 0;
}
