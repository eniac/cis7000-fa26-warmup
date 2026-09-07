/*
 * rdma_pingpong.c -- CIS 7000 warm-up, Part 2 measurements 4 and 5: RDMA,
 * two-sided and one-sided.
 *
 *   ./rdma_pingpong --server [options]              # run on the server first
 *   ./rdma_pingpong <server IP> [options]           # then the client
 *
 *   --mode two-sided     SEND / RECV ping-pong          (default)
 *   --mode read          one-sided RDMA READ of the server's memory
 *
 * Mode summary
 *
 * two-sided: the client posts a SEND. The server has a RECV posted; when the
 *   message lands its NIC writes a completion-queue entry, the server polls
 *   that queue, and posts a SEND back. run_two_sided() and wait_for_recv()
 *   contain the verbs used for each round trip.
 *
 * read: the client posts an RDMA READ naming an address and key in the
 *   server's memory (see post_read()). The server's NIC serves it from that
 *   memory over PCIe. Follow run_read_server() to see what the server process
 *   does while this is happening. It only increments a counter in the
 *   registered buffer, so the client output shows concurrent changes. The
 *   relevant permissions are set in ctx_init().
 *
 * Both modes report the full round trip. A READ is inherently a round trip:
 * the request goes out, the data comes back, and the completion lands when
 * the data is in local memory.
 *
 * m510 configuration
 *
 * The RDMA device has two ports. Port 1 is the control network, so the defaults
 * use port 2. The GIDs printed at startup should contain 10.10.1.x. A public
 * address indicates that the program is using the control network.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define DEF_PORT      9100      /* TCP port for the out-of-band handshake */
#define DEF_IB_PORT   2         /* physical port: 2 = eno1d1 on m510 */
#define DEF_SIZE      64
#define DEF_ITERS     100000
#define DEF_WARMUP    10000
#define CQ_DEPTH      256
#define RECV_BATCH    32   /* receives kept pre-posted, off the critical path */
#define SIG_EVERY     16   /* signal 1 send in N; the rest never touch the CQ */
#define BUF_LEN       4096 /* one page; the payload sits at offset 0 */

enum mode { MODE_TWO_SIDED, MODE_READ };

struct conn_info {           /* exchanged over TCP, network byte order */
    uint32_t qpn;
    uint32_t psn;
    uint32_t rkey;
    uint64_t vaddr;
    uint8_t  gid[16];
    uint16_t lid;
} __attribute__((packed));

struct ctx {
    struct ibv_context      *ctx;
    struct ibv_pd           *pd;
    struct ibv_mr           *mr;
    struct ibv_cq           *cq;
    struct ibv_qp           *qp;
    struct ibv_port_attr     portinfo;
    char                    *buf;
    size_t                   buf_len;
    int                      ib_port;
    int                      gid_idx;
    struct conn_info         local, remote;
};

static int g_size = DEF_SIZE;

static void die(const char *m) { fprintf(stderr, "error: %s (%s)\n", m, strerror(errno)); exit(1); }

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ---------------------------------------------------------------- setup ---- */

static struct ibv_device *pick_device(const char *want)
{
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    if (!list || n == 0) { fprintf(stderr, "no RDMA devices found\n"); exit(1); }
    struct ibv_device *dev = NULL;
    for (int i = 0; i < n; i++) {
        if (!want || !strcmp(ibv_get_device_name(list[i]), want)) { dev = list[i]; break; }
    }
    if (!dev) { fprintf(stderr, "RDMA device %s not found\n", want ? want : "(any)"); exit(1); }
    return dev;   /* list intentionally leaked; freed at exit */
}

static void ctx_init(struct ctx *c, const char *devname, int ib_port, int gid_idx, size_t len)
{
    memset(c, 0, sizeof(*c));
    c->ib_port = ib_port;
    c->gid_idx = gid_idx;
    c->buf_len = len;

    struct ibv_device *dev = pick_device(devname);
    c->ctx = ibv_open_device(dev);
    if (!c->ctx) die("ibv_open_device");

    if (ibv_query_port(c->ctx, ib_port, &c->portinfo)) die("ibv_query_port");
    if (c->portinfo.state != IBV_PORT_ACTIVE)
        fprintf(stderr, "warning: port %d is not ACTIVE\n", ib_port);

    c->pd = ibv_alloc_pd(c->ctx);
    if (!c->pd) die("ibv_alloc_pd");

    if (posix_memalign((void **)&c->buf, 4096, len)) die("posix_memalign");
    memset(c->buf, 0, len);

    /* Access flags on the memory region. The rkey and the buffer address are
     * sent to the peer in the TCP handshake below; the NIC enforces these
     * flags on every remote operation that names that rkey. */
    c->mr = ibv_reg_mr(c->pd, c->buf, len,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!c->mr) die("ibv_reg_mr");

    c->cq = ibv_create_cq(c->ctx, CQ_DEPTH, NULL, NULL, 0);
    if (!c->cq) die("ibv_create_cq");

    struct ibv_qp_init_attr qia = {
        .send_cq = c->cq, .recv_cq = c->cq,
        .cap = { .max_send_wr = 64, .max_recv_wr = 64,
                 .max_send_sge = 1, .max_recv_sge = 1, .max_inline_data = 256 },
        .qp_type = IBV_QPT_RC,      /* Reliable Connection: ordered delivery */
        .sq_sig_all = 0,
    };
    c->qp = ibv_create_qp(c->pd, &qia);
    if (!c->qp) die("ibv_create_qp");

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = ib_port,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ,
    };
    if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                    IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        die("ibv_modify_qp to INIT");

    union ibv_gid gid;
    memset(&gid, 0, sizeof(gid));
    if (gid_idx >= 0 && ibv_query_gid(c->ctx, ib_port, gid_idx, &gid))
        die("ibv_query_gid");

    c->local.qpn   = c->qp->qp_num;
    c->local.psn   = lrand48() & 0xffffff;
    c->local.rkey  = c->mr->rkey;
    c->local.vaddr = (uintptr_t)c->buf;
    c->local.lid   = c->portinfo.lid;
    memcpy(c->local.gid, &gid, 16);
}

/* Move the QP to RTR then RTS using the peer's parameters. */
static void ctx_connect(struct ctx *c)
{
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = c->portinfo.active_mtu,
        .dest_qp_num        = c->remote.qpn,
        .rq_psn             = c->remote.psn,
        .max_dest_rd_atomic = 1,    /* READs the peer may have in flight here */
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global = 0, .dlid = c->remote.lid, .sl = 0,
            .src_path_bits = 0, .port_num = c->ib_port,
        },
    };
    /* RoCE (Ethernet link layer) has no LID; addressing is by GID, so the
     * address handle must be global. InfiniBand examples omit this and fail
     * on Ethernet NICs. */
    if (c->portinfo.link_layer == IBV_LINK_LAYER_ETHERNET) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        memcpy(&attr.ah_attr.grh.dgid, c->remote.gid, 16);
        attr.ah_attr.grh.sgid_index = c->gid_idx;
    }
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        die("ibv_modify_qp to RTR");

    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = c->local.psn;
    attr.max_rd_atomic = 1;         /* READs we keep in flight: one at a time */
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
        die("ibv_modify_qp to RTS");
}

/* ------------------------------------------------- out-of-band handshake --- */

static int tcp_exchange(const char *server_ip, int port, struct ctx *c)
{
    int fd, one = 1;
    if (server_ip == NULL) {                       /* server: listen, accept */
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls < 0) die("socket");
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa = { .sin_family = AF_INET,
                                  .sin_port = htons(port),
                                  .sin_addr.s_addr = INADDR_ANY };
        if (bind(ls, (struct sockaddr *)&sa, sizeof(sa))) die("bind");
        if (listen(ls, 1)) die("listen");
        fd = accept(ls, NULL, NULL);
        if (fd < 0) die("accept");
        close(ls);
    } else {                                       /* client: connect */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) die("socket");
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
        if (inet_pton(AF_INET, server_ip, &sa.sin_addr) != 1) {
            fprintf(stderr, "bad server IP %s\n", server_ip); exit(1);
        }
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) die("connect");
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct conn_info out = c->local;
    out.qpn = htonl(out.qpn); out.psn = htonl(out.psn);
    out.rkey = htonl(out.rkey);
    out.vaddr = ((uint64_t)htonl((uint32_t)(c->local.vaddr >> 32)) ) |
                ((uint64_t)htonl((uint32_t)(c->local.vaddr & 0xffffffff)) << 32);
    out.lid = htons(out.lid);

    if (write(fd, &out, sizeof(out)) != (ssize_t)sizeof(out)) die("write conn_info");
    struct conn_info in;
    size_t got = 0;
    while (got < sizeof(in)) {
        ssize_t r = read(fd, (char *)&in + got, sizeof(in) - got);
        if (r <= 0) die("read conn_info");
        got += r;
    }
    c->remote.qpn  = ntohl(in.qpn);
    c->remote.psn  = ntohl(in.psn);
    c->remote.rkey = ntohl(in.rkey);
    c->remote.vaddr = ((uint64_t)ntohl((uint32_t)(in.vaddr & 0xffffffff)) << 32) |
                      ((uint64_t)ntohl((uint32_t)(in.vaddr >> 32)));
    c->remote.lid  = ntohs(in.lid);
    memcpy(c->remote.gid, in.gid, 16);
    return fd;
}

static void print_gid(const char *what, const uint8_t g[16])
{
    /* RoCEv2 IPv4 GIDs are ::ffff:a.b.c.d -- print the tail so a wrong-port
     * mistake is obvious. */
    printf("%s GID ", what);
    for (int i = 0; i < 16; i++) printf("%02x%s", g[i], i == 15 ? "" : ":");
    if (g[10] == 0xff && g[11] == 0xff)
        printf("   (IPv4 %u.%u.%u.%u)", g[12], g[13], g[14], g[15]);
    printf("\n");
}

/* ------------------------------------------------------------- posting ---- */

static void post_recv(struct ctx *c)
{
    struct ibv_sge sge = { .addr = (uintptr_t)c->buf, .length = g_size,
                           .lkey = c->mr->lkey };
    struct ibv_recv_wr wr = { .wr_id = 1, .sg_list = &sge, .num_sge = 1 }, *bad;
    if (ibv_post_recv(c->qp, &wr, &bad)) die("ibv_post_recv");
}

static void post_send(struct ctx *c, int signaled)
{
    struct ibv_sge sge = { .addr = (uintptr_t)c->buf, .length = g_size,
                           .lkey = c->mr->lkey };
    struct ibv_send_wr wr = {
        .wr_id = 2, .sg_list = &sge, .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = (signaled ? IBV_SEND_SIGNALED : 0) |
                      (g_size <= 256 ? IBV_SEND_INLINE : 0),
    }, *bad;
    if (ibv_post_send(c->qp, &wr, &bad)) die("ibv_post_send (SEND)");
}

/* One RDMA READ: `g_size` bytes from the start of the peer's buffer into the
 * start of ours. The peer's rkey and virtual address came over the TCP
 * handshake; nothing on the peer runs when this executes. */
static void post_read(struct ctx *c)
{
    struct ibv_sge sge = { .addr = (uintptr_t)c->buf, .length = g_size,
                           .lkey = c->mr->lkey };
    struct ibv_send_wr wr = {
        .wr_id = 3, .sg_list = &sge, .num_sge = 1,
        .opcode = IBV_WR_RDMA_READ,
        .send_flags = IBV_SEND_SIGNALED,    /* the completion IS the reply */
        .wr.rdma = { .remote_addr = c->remote.vaddr, .rkey = c->remote.rkey },
    }, *bad;
    if (ibv_post_send(c->qp, &wr, &bad)) die("ibv_post_send (READ)");
}

/* Block until an inbound message lands, discarding any signaled send
 * completions that show up on the way. Send completions are outside the
 * measurement: waiting on them would put the local NIC's completion path inside
 * the timed region and can inflate a two-sided reading several-fold relative to
 * perftest for the same operation. */
static void wait_for_recv(struct ctx *c)
{
    struct ibv_wc wc;
    for (;;) {
        int n = ibv_poll_cq(c->cq, 1, &wc);
        if (n < 0) die("ibv_poll_cq");
        if (n == 0) continue;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "completion error: %s (opcode %d)\n",
                    ibv_wc_status_str(wc.status), wc.opcode);
            exit(1);
        }
        if (wc.opcode == IBV_WC_RECV) return;
    }
}

static void reap_one(struct ctx *c)
{
    struct ibv_wc wc;
    int n;
    do { n = ibv_poll_cq(c->cq, 1, &wc); } while (n == 0);
    if (n < 0) die("ibv_poll_cq");
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "completion error: %s (opcode %d)\n",
                ibv_wc_status_str(wc.status), wc.opcode);
        exit(1);
    }
}

/* ------------------------------------------------------------- the loops -- */

static void run_two_sided(struct ctx *c, int is_server, long iters, long warmup,
                          uint64_t *lat)
{
    long total = iters + warmup, n = 0;

    /* Pre-post a batch of receives. Posting one inside the timed region would
     * charge every round trip for a verb call that a real server would have
     * done ahead of time. */
    for (int i = 0; i < RECV_BATCH; i++)
        post_recv(c);

    if (is_server) {
        for (long i = 0; i < total; i++) {
            wait_for_recv(c);
            post_recv(c);                             /* replenish */
            post_send(c, ((i + 1) % SIG_EVERY) == 0); /* mostly unsignaled */
        }
    } else {
        for (long i = 0; i < total; i++) {
            uint64_t t0 = now_ns();
            post_send(c, ((i + 1) % SIG_EVERY) == 0);
            wait_for_recv(c);                         /* the reply */
            uint64_t t1 = now_ns();

            post_recv(c);                             /* replenish, untimed */
            if (i >= warmup) lat[n++] = t1 - t0;
        }
    }
}

/* The server side of read mode calls no verb and never looks at the CQ. It
 * keeps a counter ticking at the start of the registered buffer so the client
 * can see that it is reading live memory. It runs until the client closes the
 * handshake socket. */
static void run_read_server(struct ctx *c, int sock)
{
    volatile uint64_t *counter = (volatile uint64_t *)c->buf;
    if (g_size > 8)
        snprintf(c->buf + 8, g_size - 8, "hello from pid %d", getpid());

    for (;;) {
        (*counter)++;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 };  /* 100 us */
        nanosleep(&ts, NULL);

        char x;
        ssize_t r = recv(sock, &x, 1, MSG_DONTWAIT);
        if (r == 0) break;                                   /* client hung up */
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
    }
    printf("server: counter reached %lu; posted no verbs\n",
           (unsigned long)*counter);
}

static void run_read_client(struct ctx *c, long iters, long warmup, uint64_t *lat)
{
    long total = iters + warmup, n = 0;
    uint64_t first = 0, last = 0;

    for (long i = 0; i < total; i++) {
        uint64_t t0 = now_ns();
        post_read(c);
        reap_one(c);              /* completion: the bytes are in c->buf */
        uint64_t t1 = now_ns();

        if (i >= warmup) {
            lat[n++] = t1 - t0;
            uint64_t v;
            memcpy(&v, c->buf, sizeof(v));
            if (n == 1) first = v;
            last = v;
        }
    }
    printf("server memory: counter %lu at first read, %lu at last "
           "(%lu increments during %ld reads)",
           (unsigned long)first, (unsigned long)last,
           (unsigned long)(last - first), iters);
    if (g_size > 8)
        printf("; text \"%.*s\"", g_size - 8, c->buf + 8);
    printf("\n");
}

/* ----------------------------------------------------------------- main --- */

static void usage(const char *p)
{
    fprintf(stderr,
      "usage:\n"
      "  %s --server [options]          run this first\n"
      "  %s <server IP> [options]\n\n"
      "  -d, --dev NAME       RDMA device (default: first found)\n"
      "  -i, --ib-port N      physical port (default %d; port 1 is the CONTROL net on m510)\n"
      "  -x, --gid-index N    GID index (default 3, RoCEv2 IPv4)\n"
      "  -m, --mode M         two-sided | read (default two-sided)\n"
      "  -s, --size B         payload bytes (default %d)\n"
      "  -n, --iters N        measured iterations (default %d)\n"
      "  -w, --warmup N       discarded iterations (default %d)\n"
      "  -p, --port N         TCP port for the handshake (default %d)\n"
      "      --out FILE       write every sample as CSV\n",
      p, p, DEF_IB_PORT, DEF_SIZE, DEF_ITERS, DEF_WARMUP, DEF_PORT);
}

int main(int argc, char **argv)
{
    const char *devname = NULL, *server_ip = NULL, *out_path = NULL;
    int ib_port = DEF_IB_PORT, gid_idx = 3, tcp_port = DEF_PORT, is_server = 0;
    long iters = DEF_ITERS, warmup = DEF_WARMUP;
    enum mode mode = MODE_TWO_SIDED;

    static struct option opts[] = {
        {"server", no_argument, 0, 'S'}, {"dev", required_argument, 0, 'd'},
        {"ib-port", required_argument, 0, 'i'}, {"gid-index", required_argument, 0, 'x'},
        {"mode", required_argument, 0, 'm'}, {"size", required_argument, 0, 's'},
        {"iters", required_argument, 0, 'n'}, {"warmup", required_argument, 0, 'w'},
        {"port", required_argument, 0, 'p'}, {"out", required_argument, 0, 'o'},
        {"help", no_argument, 0, 'h'}, {0,0,0,0}
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "Sd:i:x:m:s:n:w:p:o:h", opts, NULL)) != -1) {
        switch (ch) {
        case 'S': is_server = 1; break;
        case 'd': devname = optarg; break;
        case 'i': ib_port = atoi(optarg); break;
        case 'x': gid_idx = atoi(optarg); break;
        case 'm':
            if (!strcmp(optarg, "two-sided")) mode = MODE_TWO_SIDED;
            else if (!strcmp(optarg, "read")) mode = MODE_READ;
            else { fprintf(stderr, "bad --mode %s\n", optarg); return 1; }
            break;
        case 's': g_size = atoi(optarg); break;
        case 'n': iters = atol(optarg); break;
        case 'w': warmup = atol(optarg); break;
        case 'p': tcp_port = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        default: usage(argv[0]); return ch == 'h' ? 0 : 1;
        }
    }
    if (!is_server) {
        if (optind >= argc) { usage(argv[0]); return 1; }
        server_ip = argv[optind];
    }
    if (g_size < 1 || g_size > BUF_LEN) { fprintf(stderr, "--size out of range\n"); return 1; }

    srand48(getpid() ^ time(NULL));

    /* Open the output file first, so a path that cannot be written fails
     * here and no run is wasted. */
    FILE *out = NULL;
    if (out_path && !is_server && !(out = fopen(out_path, "w"))) die("fopen --out");

    struct ctx c;
    ctx_init(&c, devname, ib_port, gid_idx, BUF_LEN);

    const char *mode_name = mode == MODE_TWO_SIDED ? "two-sided" : "read";
    /* Identify the run in the output, so a file can always be traced back to
     * what actually ran. Keep this with your raw data. */
    {
        char ts[64]; time_t t = time(NULL);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
        printf("benchmark: rdma_pingpong\nrole: %s\nmode: %s\ndevice: %s\nib_port: %d\n"
               "gid_index: %d\npayload_bytes: %d\niters: %ld\nwarmup: %ld\ntimestamp: %s\n",
               is_server ? "server" : "client", mode_name,
               ibv_get_device_name(c.ctx->device), ib_port, gid_idx, g_size,
               iters, warmup, ts);
        if (server_ip) printf("server: %s\n", server_ip);
    }

    int sock = tcp_exchange(server_ip, tcp_port, &c);
    print_gid("local ", c.local.gid);
    print_gid("remote", c.remote.gid);
    fflush(stdout);

    /* In two-sided mode the server must have a RECV posted before the client's
     * first SEND can land, so connect the receive side first, then sync. */
    ctx_connect(&c);

    uint64_t *lat = NULL;
    if (!is_server) {
        lat = malloc(sizeof(uint64_t) * iters);
        if (!lat) die("malloc");
    }

    /* Barrier: both sides are in RTS before any traffic. */
    { char x = 'r'; if (write(sock, &x, 1) != 1) die("sync write");
      if (read(sock, &x, 1) != 1) die("sync read"); }

    if (mode == MODE_TWO_SIDED)  run_two_sided(&c, is_server, iters, warmup, lat);
    else if (is_server)          run_read_server(&c, sock);
    else                         run_read_client(&c, iters, warmup, lat);

    if (is_server) {
        printf("server: done\n");
    } else {
        double sum = 0;
        for (long i = 0; i < iters; i++) sum += lat[i];

        if (out) {
            /* Raw samples only. Compute percentiles yourself; the metadata
             * that identifies this run is on stdout. */
            fprintf(out, "sample,t_us\n");
            for (long i = 0; i < iters; i++)
                fprintf(out, "%ld,%.3f\n", i, lat[i] / 1000.0);
            fclose(out);
            fprintf(stderr, "wrote %ld samples to %s\n", iters, out_path);
        }

        qsort(lat, iters, sizeof(uint64_t), cmp_u64);
        printf("RTT us: min %.2f p50 %.2f mean %.2f p99 %.2f p99.9 %.2f max %.2f\n",
               lat[0] / 1000.0, lat[iters / 2] / 1000.0, sum / iters / 1000.0,
               lat[(long)(iters * 0.99)] / 1000.0,
               lat[(long)(iters * 0.999)] / 1000.0,
               lat[iters - 1] / 1000.0);
        free(lat);
    }

    close(sock);
    ibv_destroy_qp(c.qp); ibv_destroy_cq(c.cq);
    ibv_dereg_mr(c.mr); ibv_dealloc_pd(c.pd); ibv_close_device(c.ctx);
    free(c.buf);
    return 0;
}
