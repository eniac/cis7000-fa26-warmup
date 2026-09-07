/*
 * membw.c -- CIS 7000 warm-up, Part 4 condition C.
 *
 * A memory-bandwidth load generator. N threads stream through buffers larger
 * than the last-level cache. It is intended to load the memory controller while
 * using relatively few cores.
 *
 *   ./membw <threads> [--mode nt|rfo|read|chase] [--mb 256] [--seconds 60]
 *           [--cpus LIST] [--no-huge] [--quiet]
 *
 * Implementation notes for the m510 (Xeon D-1548, Broadwell-DE)
 *
 * L3 is 12 MB shared by all 16 logical CPUs; memory is 2-channel DDR4 with a
 * theoretical peak near 38 GB/s and a realistic streaming ceiling around
 * 20-26 GB/s.
 *
 * MODE. Default is `nt`: 16-byte non-temporal stores (_mm_stream_si128).
 * Non-temporal stores bypass the cache hierarchy. This loads the memory
 * controller without evicting the server's L3 lines, separating bandwidth from
 * cache capacity in Part 4. NT stores also avoid read-for-ownership traffic.
 * `rfo` and `read` provide cache-thrashing comparisons. `chase` is a dependent
 * pointer chase and provides a low-bandwidth comparison.
 *
 * AVX2 stores put a Broadwell core into a higher power state and reduce package
 * frequency, adding a second source of interference. At 2.0 GHz, 16-byte SSE
 * stores need about 0.28 stores/cycle to reach 9 GB/s, so SSE is enough. Keep
 * this implementation on SSE.
 *
 * THREADS. One core streaming NT stores takes a large share of the controller
 * by itself, so a few threads reach 80-90% of the achievable memory bandwidth
 * while occupying 3 of the 8 physical cores.
 *
 * HUGEPAGES. 256 MB per thread on 4 KB pages adds page walks, which generate
 * memory traffic of their own, so the buffer is requested on 2 MB hugepages,
 * with a warning if that fails.
 *
 * The program reports achieved GB/s so the antagonist's bandwidth is known.
 * Cross-check it against the memory controller itself with:
 *   sudo perf stat -a -e uncore_imc/cas_count_read/,uncore_imc/cas_count_write/ \
 *        -- sleep 5
 * Calculate bandwidth as (CAS_read + CAS_write) * 64 B / elapsed time.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <emmintrin.h>

enum mode { M_NT, M_RFO, M_READ, M_CHASE };

static volatile int running = 1;
static size_t buf_bytes = 256ull << 20;
static enum mode g_mode = M_NT;

struct targ {
	int id;
	int cpu;              /* -1 = do not pin */
	char *buf;
	uint64_t bytes;       /* bytes the thread moved */
};

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *worker(void *p)
{
	struct targ *a = p;

	if (a->cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(a->cpu, &set);
		if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
			fprintf(stderr, "warning: could not pin thread %d to cpu %d\n",
				a->id, a->cpu);
	}

	char *buf = a->buf;
	size_t n = buf_bytes;
	uint64_t moved = 0;

	if (g_mode == M_CHASE) {
		/* Build a shuffled cycle of 64-byte-spaced offsets, then chase it.
		 * One outstanding miss per thread, so it is latency-bound at ~0.8 GB/s. */
		size_t nodes = n / 64;
		size_t *idx = malloc(nodes * sizeof(size_t));
		if (!idx) return NULL;
		for (size_t i = 0; i < nodes; i++) idx[i] = i;
		for (size_t i = nodes - 1; i > 0; i--) {
			size_t j = (size_t)rand() % (i + 1);
			size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
		}
		for (size_t i = 0; i < nodes; i++)
			*(size_t *)(buf + idx[i] * 64) = idx[(i + 1) % nodes];
		free(idx);

		size_t cur = 0;
		while (running) {
			for (int k = 0; k < 1024 && running; k++) {
				cur = *(size_t *)(buf + cur * 64);
				moved += 64;
			}
		}
		a->bytes = moved;
		return NULL;
	}

	const __m128i val = _mm_set1_epi32(0x5a5a5a5a);

	while (running) {
		switch (g_mode) {
		case M_NT:
			/* Non-temporal stores go straight to memory with no RFO and no L3
			 * allocation. */
			for (size_t off = 0; off + 64 <= n; off += 64) {
				_mm_stream_si128((__m128i *)(buf + off +  0), val);
				_mm_stream_si128((__m128i *)(buf + off + 16), val);
				_mm_stream_si128((__m128i *)(buf + off + 32), val);
				_mm_stream_si128((__m128i *)(buf + off + 48), val);
			}
			_mm_sfence();
			break;
		case M_RFO:
			/* Ordinary stores: allocates in cache, evicts the victim's
			 * lines, and costs a read-for-ownership per line. */
			for (size_t off = 0; off + 64 <= n; off += 64) {
				_mm_store_si128((__m128i *)(buf + off +  0), val);
				_mm_store_si128((__m128i *)(buf + off + 16), val);
				_mm_store_si128((__m128i *)(buf + off + 32), val);
				_mm_store_si128((__m128i *)(buf + off + 48), val);
			}
			break;
		case M_READ: {
			__m128i acc = _mm_setzero_si128();
			for (size_t off = 0; off + 64 <= n; off += 64) {
				acc = _mm_add_epi32(acc,
					_mm_load_si128((const __m128i *)(buf + off)));
				acc = _mm_add_epi32(acc,
					_mm_load_si128((const __m128i *)(buf + off + 16)));
				acc = _mm_add_epi32(acc,
					_mm_load_si128((const __m128i *)(buf + off + 32)));
				acc = _mm_add_epi32(acc,
					_mm_load_si128((const __m128i *)(buf + off + 48)));
			}
			/* Keep the accumulator live so the loop is not optimized away. */
			if (_mm_cvtsi128_si32(acc) == 0x7fffffff)
				fprintf(stderr, "%s", "");
			break;
		}
		default: break;
		}
		moved += n;
	}

	a->bytes = moved;
	return NULL;
}

/* Parse "4-7,12-15" into a list of cpu ids. */
static int parse_cpus(const char *s, int *out, int max)
{
	int n = 0;
	const char *p = s;
	while (*p && n < max) {
		int a = 0, b = 0;
		if (sscanf(p, "%d-%d", &a, &b) == 2) {
			for (int c = a; c <= b && n < max; c++) out[n++] = c;
		} else if (sscanf(p, "%d", &a) == 1) {
			out[n++] = a;
		}
		while (*p && *p != ',') p++;
		if (*p == ',') p++;
	}
	return n;
}

int main(int argc, char **argv)
{
	int nthreads = 3, seconds = 60, quiet = 0, want_huge = 1;
	const char *cpulist = NULL;
	size_t mb = 256;

	static struct option opts[] = {
		{"mode",     required_argument, 0, 'm'},
		{"mb",       required_argument, 0, 'b'},
		{"seconds",  required_argument, 0, 't'},
		{"cpus",     required_argument, 0, 'c'},
		{"no-huge",  no_argument,       0, 'H'},
		{"quiet",    no_argument,       0, 'q'},
		{0, 0, 0, 0}
	};

	/* First positional argument is the thread count, matching the handout's
	 * `membw 16` usage. */
	if (argc > 1 && argv[1][0] != '-') {
		nthreads = atoi(argv[1]);
		optind = 2;
	}
	int c;
	while ((c = getopt_long(argc, argv, "m:b:t:c:Hq", opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if      (!strcmp(optarg, "nt"))    g_mode = M_NT;
			else if (!strcmp(optarg, "rfo"))   g_mode = M_RFO;
			else if (!strcmp(optarg, "read"))  g_mode = M_READ;
			else if (!strcmp(optarg, "chase")) g_mode = M_CHASE;
			else { fprintf(stderr, "bad --mode %s\n", optarg); return 1; }
			break;
		case 'b': mb = (size_t)atol(optarg); break;
		case 't': seconds = atoi(optarg); break;
		case 'c': cpulist = optarg; break;
		case 'H': want_huge = 0; break;
		case 'q': quiet = 1; break;
		default: return 1;
		}
	}
	if (nthreads < 1) nthreads = 1;
	buf_bytes = mb << 20;

	int cpus[256];
	int ncpus = cpulist ? parse_cpus(cpulist, cpus, 256) : 0;

	struct targ *args = calloc((size_t)nthreads, sizeof(*args));
	pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
	if (!args || !tids) { perror("calloc"); return 1; }

	for (int i = 0; i < nthreads; i++) {
		char *buf = MAP_FAILED;
		if (want_huge)
			buf = mmap(NULL, buf_bytes, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (buf == MAP_FAILED) {
			if (want_huge)
				fprintf(stderr, "warning: thread %d: hugepage mmap failed (%s); "
					"this thread runs on 4K pages\n", i, strerror(errno));
			if (want_huge && i == 0)
				fprintf(stderr, "  reserve more with: echo N | sudo tee "
					"/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages\n");
			buf = mmap(NULL, buf_bytes, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		}
		if (buf == MAP_FAILED) { perror("mmap"); return 1; }
		memset(buf, 0, buf_bytes);     /* fault it in before timing */

		args[i].id = i;
		args[i].buf = buf;
		args[i].cpu = ncpus ? cpus[i % ncpus] : -1;
	}

	if (!quiet) {
		const char *mn = g_mode == M_NT ? "nt" : g_mode == M_RFO ? "rfo" :
				 g_mode == M_READ ? "read" : "chase";
		printf("membw: %d threads, %zu MB/thread, mode=%s, %d s",
		       nthreads, mb, mn, seconds);
		if (cpulist) printf(", cpus=%s", cpulist);
		printf("\n");
		fflush(stdout);
	}

	uint64_t t0 = now_ns();
	for (int i = 0; i < nthreads; i++)
		pthread_create(&tids[i], NULL, worker, &args[i]);

	struct timespec ts = { .tv_sec = seconds, .tv_nsec = 0 };
	nanosleep(&ts, NULL);
	running = 0;

	uint64_t total = 0;
	for (int i = 0; i < nthreads; i++) {
		pthread_join(tids[i], NULL);
		total += args[i].bytes;
	}
	double secs = (now_ns() - t0) / 1e9;

	if (!quiet)
		printf("membw: moved %.1f GB in %.1f s = %.2f GB/s\n",
		       total / 1e9, secs, total / 1e9 / secs);

	for (int i = 0; i < nthreads; i++) munmap(args[i].buf, buf_bytes);
	free(args); free(tids);
	return 0;
}
