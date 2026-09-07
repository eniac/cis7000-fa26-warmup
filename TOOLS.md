# CIS 7000 warm-up: Provided tools

This repository contains the benchmark programs and setup scripts for the warm-up assignment.
You will write the experiment harness and analysis code.

## Provided

| Path | Purpose |
|---|---|
| `rtt/echo_server`, `rtt/echo_client` | TCP request/response round-trip benchmark. `--spin` on both ends polls the socket in a loop; the default blocks. The client writes one row per sample to `--out`. Both print how much CPU time they consumed. |
| `rdma/rdma_pingpong` | RDMA round-trip benchmark. `--mode two-sided` is SEND/RECV; `--mode read` is a one-sided READ of the server's registered buffer. The client writes one row per sample to `--out`. The server serves one client run and exits. |
| `antagonist/membw` | Memory-bandwidth workload. `membw <threads> --mode nt --seconds N`. Prints the bandwidth it reached. |
| `scripts/setup.sh` | Prepares a stock CloudLab node by installing packages and mcperf, configuring perf permissions and hugepages, and running a checklist. Run it once per node with sudo before `make`. It does not tune the node. |
| `scripts/check-system.sh` | Prints the machine state relevant to latency measurement. Read-only. Run it before and after `tune.sh` and keep both outputs. |
| `scripts/tune.sh` | Configures a node for repeatable low-latency measurement. Each operation prints its before and after value. The settings apply to the running kernel and NIC driver and are lost on reboot. |
| `scripts/set_irq_affinity.sh` | Finds the experiment NIC's receive-queue interrupt vectors and assigns each one CPU from the given list, printing every vector's affinity before and after. |

## Flags

| Program | Flags (defaults in brackets) |
|---|---|
| `echo_server` | `--bind IP` [0.0.0.0], `--port N` [9000], `--spin`, `--quiet` |
| `echo_client` | `<server IP>`, `--port N` [9000], `--size B` [64], `--samples N` [100000], `--warmup N` [10000], `--spin`, `--pace-us U` [0: back to back; a pacing interval spreads the run over wall-clock time so slow tail sources such as timer ticks are sampled], `--out FILE`, `--label NAME` |
| `rdma_pingpong` | `--server` or `<server IP>`, `--mode two-sided\|read` [two-sided], `--size B` [64], `--iters N` [100000], `--warmup N` [10000], `--out FILE`, `--dev NAME`, `--ib-port N` [2], `--gid-index N` [3], `--port N` [TCP handshake port]; give both sides the same `--iters`, `--warmup` and `--size` |
| `membw` | `<threads>` [3], `--mode nt\|rfo\|read\|chase` [nt], `--mb N` [256 per thread], `--seconds N` [60], `--cpus LIST`, `--no-huge`, `--quiet` |

Source for every program is next to its binary (`rtt/*.c`, `rdma/*.c`, `antagonist/*.c`); `make`
rebuilds them. Also installed: `memcached`, `mcperf`, `stress-ng`, `pidstat` and `mpstat`
(sysstat), `perf`, `ib_send_lat`/`ib_read_lat` (perftest), and `python3` with numpy, pandas and
matplotlib.

## Files you write

- the harness that runs repeated experiments and preserves every raw output
- the organisation of your raw data
- the processing that turns raw samples into percentiles
- the plotting, and a `Makefile` whose `make figures` regenerates every figure from `data/`

The provided scripts do not perform these tasks.

## Output formats

`~/warmup` is your clone of the course repository. Keep your scripts there, and run
`git pull && make` on both nodes when an update is announced. Keep raw data outside the repository;
the handout uses `~/data/`. Both benchmarks exit if they cannot create the `--out` file.

**Latency CSVs** (`echo_client --out`, `rdma_pingpong --out`) hold raw samples, one per row, in
microseconds. `rdma_pingpong` writes two columns; `echo_client` adds `t_offset_us`, the sample's
start time from the beginning of the run:

```
sample,t_us                 sample,t_offset_us,t_us
0,3.482                     0,0.000,46.912
1,3.479                     1,58.310,47.101
```

Percentiles are yours to compute. Both programs print a block of metadata to stdout that
identifies the run; capture stdout alongside the CSV. `echo_client` prints `benchmark:`, `role:`,
`mode:`, `server:`, `payload_bytes:`, `samples:`, `warmup:`, `label:`, `timestamp:`.
`rdma_pingpong` prints `benchmark:`, `role:`, `mode:`, `device:`, `ib_port:`, `gid_index:`,
`payload_bytes:`, `iters:`, `warmup:`, `timestamp:` and, on the client, `server:`.

**mcperf** prints its results to stdout. The lines to parse are the `Total QPS = ...` line and the
`read` row of the latency table, whose header names the columns:

```
#type       avg     std     min      p5     p10     p50     p67     p75     p80     p85     p90     p95     p99    p999   p9999
read      178.3    70.6    40.2    68.6    89.6   169.9   203.3   223.2   236.3   252.3   271.8   301.5   369.0   513.3   623.5
Total QPS = 99924.5 (1998491 / 20.0s)
```

Run it once and read the header before writing a parser. `-q <rate>` sets an offered rate
in requests per second; `-d <depth>` is the number of requests one connection may have
outstanding (default 1, which throttles the schedule under overload; the handout uses 4096 so the
schedule holds). `-q 0` removes the rate limit: each connection sends its next request as soon as
the previous reply arrives.

**memcached** reports its own counters over the network:

```
printf 'stats\r\nquit\r\n' | nc 10.10.1.1 11211
```

`STAT bytes` is the dataset size; `rusage_user` and `rusage_system` are memcached's cumulative
CPU seconds.

## Hardware notes

The control interface you ssh over (`eno1`) and the experiment interface (`eno1d1`, 10.10.1.x)
are two ports of one ConnectX-3. `rdma link show` lists them as port 1 and port 2 of the same
device. `rdma_pingpong` uses port 2 by default and prints the GIDs it chose; they should contain
your `10.10.1.x` addresses. perftest defaults to port 1, so pass `-i 2`:

```
IB_DEV=$(ibv_devinfo -l | awk 'NR>1 && NF {gsub(/[ \t]/, ""); print; exit}')
ib_read_lat -d $IB_DEV -i 2 -s 64 -n 100000        # server first, then client with the server IP
ib_send_lat -d $IB_DEV -i 2 -s 64 -n 100000
```

The RDMA device is named `mlx4_0` on a fresh boot and `roceo1` after `rdma-core`'s udev rules run,
so that line discovers the name at run time.

Logical CPUs 8–15 are the second hardware thread of physical cores 0–7. `check-system.sh` prints
the full sibling map.
