# CIS 7000: Warm-up assignment

**Latency, load, and interference on two bare-metal machines**

*Assigned Mon Sep 7 · Due Tue Sep 22, 11:59 pm ET · 5% of the course grade · Individual*

---

## Purpose

This assignment introduces systems experiments on real machines. You will use two bare-metal
CloudLab servers to measure several network paths, characterize latency near a server's capacity,
add competing workloads, and test CPU isolation.

Power management, interrupt moderation, scheduling, hardware threads, and queueing can all affect
latency. In some cases, these effects are larger than the one you meant to measure. You will need
to document the experimental setup and explain the results you obtain. The datapath programs are
provided. You will write the experiment automation, save the raw data, make predictions before
each set of measurements, and design one follow-up experiment.

Plan for 8 to 12 hours, which includes unattended measurement time. Start early and script repeated
experiments.

---

## Part 0: Getting a machine

### CloudLab account

1. Register at [cloudlab.us](https://www.cloudlab.us/) with your Penn email. Upload an SSH public
   key during registration; without one you cannot log in.
2. Join the project **`CIS-7000-fa26`**. Approval is manual, so do this as soon as the assignment
   is posted. If you are not approved within 24 hours, post on Piazza.

### Instantiating the experiment

Instantiate the profile **`CIS7000-warmup`** with its default parameters.

It gives you two bare-metal **m510** nodes:

- `server`
- `client`

They are connected by a dedicated 10 Gb link and boot CloudLab's stock Ubuntu 24.04 image.
Everything else the assignment needs comes from the repository below.

Instantiation usually takes 5–15 minutes. Once the experiment turns green, the *List View* tab
gives you the SSH commands.

### Getting the code

The benchmark programs and helper scripts live in this git repository. On **both** nodes:

```bash
git clone https://github.com/eniac/cis7000-fa26-warmup.git ~/warmup
cd ~/warmup
sudo ./scripts/setup.sh
make
```

`setup.sh` takes a few minutes. It installs the required packages (memcached, stress-ng, perf,
sysstat, the RDMA userspace, tmux, and htop), builds `mcperf`, and runs a verification checklist.

`~/warmup` contains your copy of the repository. The `README.md` file contains this handout, and
`TOOLS.md` describes the provided programs. Save your scripts here. If an update is posted on Piazza, run `git pull` and `make` on both nodes.
Run all relative commands below from `~/warmup`.

### Working on the nodes

Most experiments need a server process, a client process, and one or more monitoring commands.
Use `tmux` to keep several shells open on each node and to keep processes running if SSH
disconnects. The commands below are enough for this assignment. Start one session per node:

```bash
tmux new -s work
```

Everything in tmux starts with the prefix `Ctrl-b`, released before the next key:

| keys | what happens |
|---|---|
| `Ctrl-b "` | split the current pane top/bottom |
| `Ctrl-b %` | split the current pane left/right |
| `Ctrl-b` then an arrow key | move to the pane in that direction |
| `Ctrl-b z` | zoom the current pane to full screen; again to unzoom |
| `Ctrl-b x` | close the current pane |
| `Ctrl-b c` | new window (a full-screen tab); `Ctrl-b n` / `Ctrl-b p` move between windows |
| `Ctrl-b [` | scroll back through output; arrow keys or PageUp, `q` to leave |
| `Ctrl-b d` | detach; the session and everything in it keeps running |
| `tmux attach -t work` | re-attach after a detach or a dropped connection |

A useful layout is one tmux session per node, with one pane for the benchmark and one for the
`htop` tool.

Stop a server program with Ctrl-C in its window, or from another shell with
`pkill -x echo_server` (or `memcached`, `rdma_pingpong`, `stress-ng`, `membw`). Before every
measurement, check that nothing from the previous one is still running: `htop` shows all the
running processes, you can also check for a leftover process using `pgrep -a memcached` (or
another name) to confirm it.

To watch what the machine is doing during a run, keep `htop` or `mpstat 1` in its own pane.

Copy your raw data off both nodes at the end of every session (Part 1 creates `~/data` on
each), for example from your laptop:

```bash
scp -r <user>@<client-node>:data ./data-client
scp -r <user>@<server-node>:data ./data-server
```

CloudLab notes:

- **Experiments expire.** CloudLab experiments have a default 16-hour lease, but request only
  the time you expect to use. Extend the lease from the experiment page if necessary.
- **The class shares a limited pool.** The course reservation guarantees 15 m510 nodes for the
  entire class, and each experiment uses two. An idle experiment prevents other students from
  using those nodes. Current resource availability is shown on the [CloudLab Resource Availability page](https://www.cloudlab.us/resinfo.php).
- **Terminate experiments promptly.** Terminate your experiment as soon as you finish working.
  You can start a new one for your next session; cloning the repository and running setup.sh
  takes less than twenty minutes.
- **Copy your work first.** Local disks are deleted when an experiment is terminated. Copy
  your raw data and code off both nodes before terminating it.

---

### Your hardware

The m510 has:

- Intel Xeon D-1548, 2.0 GHz
- 8 physical cores / 16 logical CPUs
- 64 GB DDR4
- 256 GB NVMe
- dual-port Mellanox ConnectX-3 10 GbE
- 12 MB shared last-level cache

Verify the topology and hardware on your assigned machine.

Record:

- CPU model
- physical and logical core count
- memory size
- NIC model
- kernel version
- OS image

Also inspect:

```bash
lscpu
ip -br addr
rdma link show
sudo ethtool eno1d1
```

The experiment network uses:

- server: `10.10.1.1`
- client: `10.10.1.2`

Every measured networking experiment in this assignment must use `10.10.1.x`.

SSH uses the other port on the same ConnectX-3 NIC. Some networking tools default to that port,
so verify that each experiment uses the `10.10.1.x` network.

### Hardware threads

Inspect:

```console
$ cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
0,8
```

`check-system.sh` (Part 1) prints the sibling of every logical CPU. Record the map and check
it against this line. You will need it later.

---

## Part 1: Stock and tuned Linux

First measure the stock CloudLab configuration.

Keep raw data outside the code tree. Create a directory for it on both nodes and put every
`--out` file below in that directory:

```bash
mkdir -p ~/data
```

Store ping output, latency CSVs, and mcperf output in the client's `~/data`. Store
`check-system.sh` snapshots, server CPU output, and the Part 4 and 5 pidstat and perf files in the
server's `~/data`. Copy both directories off the nodes after each session.

Record the machine state next. `check-system.sh` reports the CPU frequency governor, turbo and
idle-state settings, NIC interrupt coalescing and queue count, IRQ placement, RDMA device, kernel,
and OS. It is read-only. Save its output before and after tuning. On both machines:

```bash
cd ~/warmup
./scripts/check-system.sh > ~/data/check-system-stock.txt
```

Collect a short baseline from the **client**, targeting the server:

```bash
sudo ping -c 10000 -i 0 -s 56 10.10.1.1 | tee ~/data/ping_stock.txt | tail -1
```

`ping` prints one line per round trip, in **milliseconds**, and a summary with only min, avg
and max. Percentiles come from the saved lines; this converts to microseconds and prints p50 and
p99:

```bash
awk -F'time=' '/time=/{print $2*1000}' ~/data/ping_stock.txt | sort -n \
    | awk '{a[NR]=$1} END{print "p50", a[int(NR*0.50)], "p99", a[int(NR*0.99)]}'
```

Then, with the server program in a tmux pane on the server node:

```bash
# server
./rtt/echo_server --bind 10.10.1.1
# client
./rtt/echo_client 10.10.1.1 --size 64 --samples 20000 --warmup 2000 \
    --label tcp_stock --out ~/data/tcp_stock.csv
```

`echo_client` prints its p50 and p99. Record both numbers for both measurements.

Apply the low-latency configuration on **both** nodes:

```bash
sudo ./scripts/tune.sh eno1d1 4 0-3
```

Repeat the two measurements, with new file names so the stock records survive:

```bash
# client
sudo ping -c 10000 -i 0 -s 56 10.10.1.1 | tee ~/data/ping_tuned.txt | tail -1
# server
./rtt/echo_server --bind 10.10.1.1
# client
./rtt/echo_client 10.10.1.1 --size 64 --samples 20000 --warmup 2000 \
    --label tcp_tuned --out ~/data/tcp_tuned.csv
```

The tuning script changes several mechanisms, including CPU power management, NIC interrupt
moderation, and IRQ placement. Read it: each step is a few lines, prints the setting's value
before and after, and names the file or command it uses. Save `check-system.sh` output again,
as `~/data/check-system-tuned.txt`.

### Isolate one tuning change

Choose the tuning change that you think explains the largest part of the difference and state
your hypothesis before collecting more data. Restore only that setting to its stock value on both
nodes, leave the other settings tuned, and repeat the `ping` measurement. Use the `before:` lines
from `tune.sh`, the per-vector output from `set_irq_affinity.sh`, and
`check-system-stock.txt` to find the stock values. In the table below, replace values in angle
brackets with those values; do not type the brackets.

| setting | revert with |
|---|---|
| CPU governor | `for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo <governor> \| sudo tee $f; done` where `<governor>` is the name from the `before:` line, such as `powersave` (not the accompanying CPU count) |
| turbo | `echo <no_turbo> \| sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo` where `<no_turbo>` is the stock value, normally `0` |
| idle states | `sudo kill $(cat /run/cis7000-pmqos.pid)` (ends the process holding `/dev/cpu_dma_latency`) |
| irqbalance | Restore the stock service state: `sudo systemctl start irqbalance` if the `before:` line says `active`; otherwise leave it stopped. |
| NIC interrupt moderation | `sudo ethtool -C eno1d1 adaptive-rx <on-or-off> rx-usecs <rx-usecs> rx-frames <rx-frames>`, using all three values from the `before:` line |
| NIC receive queues | Check the stock `queues (current / max)` block. If it reports separate RX and TX queues, run `sudo ethtool -L eno1d1 rx <rx> tx <tx>`; if it reports Combined queues, run `sudo ethtool -L eno1d1 combined <combined>`. This resets the link, so wait a few seconds. |
| IRQ placement | Write each vector's printed `before` CPU list back with `echo <cpu-list> \| sudo tee /proc/irq/<irq>/smp_affinity_list`. Use this option only if the script printed the before value for every vector. |

Report the three ping p50 values (stock, tuned, tuned with your one setting reverted) and
whether they support the hypothesis. Then run `tune.sh` again to restore everything.

Run `tune.sh` at the beginning of each later measurement session. Its settings do not survive a
reboot.

---

## Part 2: Where the microseconds go

Measure the round-trip time of a 64-byte request using five mechanisms. For each one, make a
prediction, collect the measurements, and explain the result.

Before running any of the five measurements:

1. Rank them from lowest to highest expected median RTT.
2. Predict which adjacent pair will have the largest gap.
3. For each measurement, write one sentence naming the work you think is on the critical path.

Do this before seeing the results.

Take 100,000 measured samples after 10,000 warm-up samples unless otherwise noted.

Record all five RTT distributions on the **client**. Measurements 2–5 also require the listed
program on the server, running in a tmux pane.

### Measurement 1: ICMP

On the client, 110,000 round trips so that 100,000 remain after the warm-up:

```bash
sudo ping -c 110000 -i 0 -s 56 10.10.1.1 | tee ~/data/icmp.txt | tail -1
```

Keep `icmp.txt` as the raw record. For the distribution, convert to microseconds and drop the
first 10,000 as warm-up:

```bash
awk -F'time=' '/time=/{print $2*1000}' ~/data/icmp.txt | tail -n +10001 > ~/data/icmp_us.txt
```

### Measurement 2: TCP echo with blocking sockets

```bash
# server
./rtt/echo_server --bind 10.10.1.1
# client
./rtt/echo_client 10.10.1.1 --size 64 --samples 100000 --warmup 10000 \
    --label tcp_blocking --out ~/data/tcp_blocking.csv
```

There is one request outstanding at a time.

### Measurement 3: TCP echo with spinning

```bash
# server
./rtt/echo_server --bind 10.10.1.1 --spin
# client
./rtt/echo_client 10.10.1.1 --spin --size 64 --samples 100000 --warmup 10000 \
    --label tcp_spin --out ~/data/tcp_spin.csv
```

The sockets are non-blocking, and both processes poll for progress in a loop and never
sleep.

### Measurement 4: RDMA SEND / RECV

```bash
# server
./rdma/rdma_pingpong --server --mode two-sided
# client
./rdma/rdma_pingpong 10.10.1.1 --mode two-sided --size 64 \
    --iters 100000 --warmup 10000 --out ~/data/rdma_two_sided.csv
```

`rdma_pingpong --server` serves one client run and then exits. Start it again before every
client run, and give both sides the same `--iters`, `--warmup` and `--size`; with different
values the two sides wait for each other forever.

Read the relevant portion of `rdma_pingpong` before explaining the result.

### Measurement 5: RDMA READ

```bash
# server
./rdma/rdma_pingpong --server --mode read
# client
./rdma/rdma_pingpong 10.10.1.1 --mode read --size 64 \
    --iters 100000 --warmup 10000 --out ~/data/rdma_read.csv
```

The client prints the first and last values it reads from the registered server buffer.

Find the calls that establish remote access. In particular, find:

- the permissions passed to `ibv_reg_mr`
- the queue pair access flags

Use the code and your measurements to determine what work the server does for each operation.

### Estimate the physical lower bound

Estimate the hardware-only lower bound for one 64-byte round trip. Present one row per term, with
the time in microseconds and a brief source for the estimate.

Do it for the TCP echo request (Measurements 2 and 3); the ICMP and RDMA packets differ only in
their headers.

| term | how to estimate it |
|---|---|
| bytes on the wire | 64 B payload, the 4 B length prefix `echo_client` adds, TCP header (20 B, or 32 B with timestamps; check `/proc/sys/net/ipv4/tcp_timestamps`), IP 20 B, Ethernet 14 B + 4 B FCS, and the 8 B preamble and 12 B inter-frame gap the link adds; the reply is the same size |
| serialization | the time to clock the frame onto the wire one bit at a time. Multiply the wire bytes by 8 to get bits, then divide by the link rate of 10 Gb/s (10e9 bits per second). About 128 B at 10 Gb/s is roughly 0.1 µs. Count it once for the request and once for the reply. It is charged again at each store-and-forward hop, which the switch row covers. |
| propagation | about 5 ns per meter of cable; assume 20 m each way, node to switch to node |
| switch | one crossing per direction; a cut-through switch forwards after the header, a store-and-forward switch after the whole frame; look up a typical figure for a 10 GbE switch and say which kind you assumed |
| NIC | the MAC/PHY and DMA of one NIC add a few hundred nanoseconds; count the sending NIC and the receiving NIC in each direction |

Sum the rows and identify the largest term. The total should be a few microseconds.

Compare this floor to your five measured medians.

### Deliverable

**Figure 1.** Five mechanisms, with p50 and p99 RTT for each. Use a log y-axis. Draw your
estimated physical floor as a horizontal line.

Under the figure, give your original ordering and briefly discuss any result that differed from
your prediction.

---

## Part 3: Load, queueing, and the knee

Measure how latency changes as the server approaches its throughput limit.

### Start memcached

On the **server**, in its own tmux pane (it runs until you `pkill -x memcached`):

```bash
taskset -c 0-3 memcached -t 4 -m 4096 -c 32768 -l 10.10.1.1 &
```

This places memcached on logical CPUs 0–3. Check that exactly one memcached is running:
`pgrep -x memcached | wc -l` must print 1.

Load the dataset. On the **client**:

```bash
mcperf -s 10.10.1.1 --loadonly -r 1000000 -K fb_key -V fb_value
```

Confirm the resulting dataset size, from the client:

```bash
printf 'stats\r\nquit\r\n' | nc 10.10.1.1 11211 | grep 'STAT bytes '
```

memcached keeps the dataset in memory only. Every time you start memcached, load the dataset
again before measuring anything, and check `STAT bytes` again.

`-r 1000000` sets the key range for both loading and measurement. Without it, mcperf uses its
default range of 10,000 keys, and the requested data may fit in cache even if you loaded more.
Include `-r 1000000` in every remaining `mcperf` command.

### Estimate capacity

On the **client**, start with:

```bash
mcperf -s 10.10.1.1 --noload -r 1000000 -K fb_key -V fb_value -T 8 -c 16 -q 0 -t 30
```

`-T 8 -c 16` creates 8 threads with 16 connections each, for 128 total connections. Use these
values for every memcached measurement. `-q 0` removes the rate limit: each connection sends its
next request after receiving the previous reply. This is a closed-loop workload, so server
response time determines the offered load.

`-q N` schedules a total of N requests per second across the connections, independently of server
replies. The `-d` flag controls how many requests each connection may have outstanding. Its default
of 1 throttles the schedule during overload because a connection must wait for a reply. Setting
`-d 4096` avoids that throttle and produces an open-loop workload; excess requests queue at the
server. Use the following options for every rate-driven run:

```bash
mcperf -s 10.10.1.1 --noload -r 1000000 -K fb_key -V fb_value -T 8 -c 16 -d 4096 -q N -t 20
```

`-t` is the run length in seconds; use 20 for every rate-driven run.

Use these runs to get a rough estimate of the server's sustainable throughput. The repeated sweep
below determines the value you will report.

A useful starting range is:

150k–400k requests/s

but do not assume those numbers bracket the knee on your machine.

Watch CPU utilization on both nodes while doing this:

```bash
mpstat 1
```

### Write your experiment harness

Use a script for every repeated run from this point on.

Write a script that:

- accepts an offered request rate
- runs mcperf with `-r 1000000 -T 8 -c 16 -d 4096 -t 20`
- records the complete raw output
- extracts at least offered load, achieved load, p50, p99, and p99.9
- records a timestamp and enough metadata to identify the run
- never silently overwrites an earlier run

Use that script to measure 12–15 load points total. Include points from low load through about 98%
of your initial throughput estimate, plus two or three points above it near 110%, 130%, and 160%.

Take three repetitions per point.

Spend more points near the knee than at low load.

The exact set of offered rates is your choice. After the sweep, define the **sustainable peak** as
the highest offered rate for which the median achieved QPS across the three repetitions is within
2% of the offered rate. Use this definition everywhere the rest of the assignment refers to the
sustainable peak or capacity.

Above capacity, the server queue grows for the duration of the run. These latency measurements
are not steady-state results and depend on `-t`. Report them and explain that dependence.

### Deliverables

**Figure 2.** Achieved throughput on the x-axis and latency on a log y-axis. Plot p50, p99, and
p99.9. For each offered rate, use the median achieved QPS as the x-coordinate and the median
latency as the y-coordinate. Show the latency range across the three repetitions with vertical
error bars or faint individual points. Points above capacity should cluster near the same achieved
throughput while latency rises.

**Figure 3.** Offered load on the x-axis and achieved load on the y-axis. At each offered rate,
plot the median achieved load and show its minimum-to-maximum range across repetitions. Include
*y = x*.

Determine, using the lowest point of your sweep as the reference (it should be at or below
10% of the sustainable peak):

- the first measured offered load at which the median p99 reaches 2× its value at that lowest point
- the corresponding first measured point for median p50

Report each as an offered rate and as a fraction of the sustainable peak. Do not interpolate
between measured points.

### Closed-loop comparison

The first `-q 0` run was closed loop: each of the 128 connections waits for a reply before sending
again, so at most 128 requests are active. The sweep was open loop because `-q N -d 4096` schedules
requests independently of replies. After the sweep, repeat the closed-loop run so that you have
one measurement before and one after it:

```bash
mcperf -s 10.10.1.1 --noload -r 1000000 -K fb_key -V fb_value -T 8 -c 16 -q 0 -t 30
```

Compare the closed-loop throughput with the capacity your open-loop sweep found, and its latency
with the open-loop points on either side of that throughput.

Report both differences and explain how the workload model changes the observed behavior.

### Structural check: two server cores

Stop memcached and start it on two cores with two threads, then reload the dataset:

```bash
# server
pkill -x memcached
taskset -c 0-1 memcached -t 2 -m 4096 -c 32768 -l 10.10.1.1 &
# client
mcperf -s 10.10.1.1 --loadonly -r 1000000 -K fb_key -V fb_value
```

Use your harness to run a smaller open-loop sweep around the expected two-core capacity. Use three
repetitions near the boundary and the same 2% rule to find the sustainable peak. Do not use a
`-q 0` run as the capacity measurement.

If the two-core sustainable peak is close to the four-core result, use `mpstat 1` on the client
during the sweep to check whether the load generator is the bottleneck, and explain what you find.

Then put memcached back on four cores and reload the dataset before Part 4:

```bash
# server
pkill -x memcached
taskset -c 0-3 memcached -t 4 -m 4096 -c 32768 -l 10.10.1.1 &
# client
mcperf -s 10.10.1.1 --loadonly -r 1000000 -K fb_key -V fb_value
```

---

## Part 4: Competing workloads

Choose a fixed offered load equal to approximately 50% of the sustainable peak you found in
Part 3.

At this load, memcached should be well below its knee when running alone.

Before running anything, predict which antagonist will hurt latency more and which one will
change memcached's reported CPU utilization more.

Measure each condition three times in Parts 4 and 5. For each antagonist repetition, start the
antagonist on the server and wait three seconds. Then start the 20-second mcperf run on the client
and both server-side collectors. The antagonist runs for 60 seconds so that it covers the entire
measurement. Stop it with `pkill` before the next repetition, then start a new instance. For the
alone condition, start the client and collectors together without an antagonist.

### A: Memcached alone

Use this condition as the baseline. Run your Part 3 harness at the fixed offered load for 20
seconds with no antagonist. Collect pidstat and perf at the same time, using the commands below,
and repeat the condition three times.

### B: CPU competitor

On the server, alongside memcached:

```bash
stress-ng --cpu 16 --cpu-method ackermann --timeout 60s
```

### C: Memory competitor

On the server:

```bash
~/warmup/antagonist/membw 16 --mode nt --seconds 60
```

With 16 threads it also occupies every logical CPU. Part 5 separates that from its memory
traffic. If an antagonist pushes the server's capacity below the offered load, the queue grows
for the whole run, as it did past capacity in Part 3; report the result and say so.

### For every condition, collect

- achieved QPS
- p50
- p99
- p99.9
- memcached CPU utilization

using, on the server, for the 20 s of the run:

```bash
pidstat -p $(pgrep -x memcached) 1 20 > ~/data/pidstat-<condition>-<rep>.txt
```

Use the final `Average` row's `%CPU` value. This is memcached's total across its threads, so it can
reach 400 with four threads. Also collect:

```bash
perf stat -e cycles,instructions,cache-references,cache-misses \
    -p $(pgrep -x memcached) -o ~/data/perf-<condition>-<rep>.txt -- sleep 20
```

including enough counters to compute or report:

- IPC
- cycles
- cycles per completed request (cycles over the 20-second perf interval, divided by the completed
  request count in mcperf's `Total QPS` line; start perf and mcperf together)

If another counter would help explain what you see, collect it.

---

## Part 5: Isolation and controls

Change CPU placement and try to recover the memcached-alone result.

Keep the NIC receive-queue interrupts on memcached's CPUs. `tune.sh` initially assigns one queue
to each CPU in 0-3. The command below repeats that assignment and prints each vector's affinity
before and after the change. It discovers vectors with a traffic probe, so run it on the server
between measurements. Control-network traffic can add an unrelated vector to the output; check
the printed vector numbers.

```bash
sudo ./scripts/set_irq_affinity.sh eno1d1 0-3
```

### Experiment 5a

Keep memcached on CPUs 0–3. Run a four-thread antagonist on CPUs 4–7, with one thread per CPU.
Measure the CPU and memory antagonists separately; do not run them at the same time. Repeat
each condition three times and record your prediction before each set of runs.

Isolated CPU antagonist, on the server:

```bash
taskset -c 4-7 stress-ng --cpu 4 --cpu-method ackermann --timeout 60s
```

Isolated memory antagonist, on the server:

```bash
taskset -c 4-7 ~/warmup/antagonist/membw 4 --mode nt --seconds 60
```

### Experiment 5b

Experiment 5a changed two variables:

- where the competitor ran
- how many competitor threads were running

Add a four-thread **co-located** control for each antagonist. Keep memcached on CPUs 0–3 and run:

```bash
taskset -c 0-3 stress-ng --cpu 4 --cpu-method ackermann --timeout 60s
```

and, as a separate condition:

```bash
taskset -c 0-3 ~/warmup/antagonist/membw 4 --mode nt --seconds 60
```

Run three repetitions of each control. Use these comparisons:

- Compare the four-thread co-located condition with the four-thread isolated condition from 5a
  to measure the effect of CPU placement at a fixed thread count.
- Compare the full 16-thread condition from Part 4 with the four-thread co-located condition to
  estimate the effect of reducing antagonist parallelism while retaining direct CPU contention.

State the result of both comparisons for both antagonists.

### Deliverable

**Figure 4.** Compare:

- alone
- full CPU antagonist
- full memory antagonist
- four-thread isolated CPU antagonist
- four-thread isolated memory antagonist
- four-thread co-located CPU antagonist
- four-thread co-located memory antagonist

Show p50, p99, and p99.9. Use a log y-axis. Plot the median of the three repetitions and show the
minimum-to-maximum range with error bars or faint individual points.

Also report the median memcached CPU utilization and its minimum-to-maximum range for each
condition.

---

## Part 6: Follow up on one result

Choose one result from Parts 3 through 5 that your initial model does not fully explain.

Examples include:

- latency remains worse after CPU isolation
- an antagonist changes latency without much change in CPU utilization
- p99 and p50 respond differently
- a performance counter does not move the way you expected
- the isolated CPU and memory antagonists behave differently
- another result in your data that differs from your prediction

Write at least two possible explanations, then design and run one additional experiment that helps
distinguish between them.

You may use tools already installed on the machine, including Linux performance counters, CPU
placement, thread counts, hardware-thread placement, workload parameters, or a short additional
benchmark.

The experiment should provide evidence that favors one explanation. You do not need to settle the
question completely.

Document:

1. the observation
2. your two hypotheses
3. the experiment you chose
4. what result each hypothesis led you to expect
5. what happened
6. what you now believe

Report the result even if it rejects your hypothesis.

---

## Submission

Submit:

`<pennkey>-warmup.tar.gz`

containing the following.

### `report.pdf`

Target length: 2–3 pages of text. Figures and references may appear on additional pages.

Use these sections:

1. **Machine and setup.** Hardware, OS, kernel, network, CPU topology, and the tuning you
   applied. Include the short stock-vs-tuned result from Part 1 and your explanation.
2. **Network latency.** Figure 1, your predictions, the physical-floor calculation, and the main
   differences between the five mechanisms.
3. **Load and queueing.** Figures 2 and 3. Identify the knee and compare p50 with tail latency.
   Include the closed-loop comparison.
4. **Interference and isolation.** Figure 4. Explain what each antagonist did and what your
   isolation/control experiment establishes.
5. **Your experiment.** The open-ended Part 6 investigation.
6. **Short answers.** Answer the questions below concisely, and state what you used AI tools
   for.

### `data/`

All raw measurement output.

Do not edit raw output after collection.

### `code/`

Everything you wrote or changed.

At minimum, this should include the scripts you used to run repeated experiments and generate
your figures. Put the default plotting entry point at `code/plot.py`, or change `PLOT` in the
top-level Makefile to the path and command you use.

### `Makefile`

Running:

```bash
make figures
```

must regenerate every figure in your report from the contents of `data/`.

The tarball must unpack into this layout:

```text
report.pdf
Makefile
data/
code/
```

The shipped `Makefile` has a `figures` target that runs `$(PLOT) $(DATA) $(FIGURES)`, with `PLOT`
defaulting to `python3 code/plot.py`. Keep it, extend it, or replace it, as long as `make figures`
works from the unpacked tarball root without moving files or installing unlisted dependencies.

The final project will have the same requirement.

---

## Questions

Keep each answer to roughly a paragraph.

1. **What costs are you measuring?** Using your Part 2 results, explain why the physical
   wire-time floor is much smaller than the RTT of an ordinary network request. Then explain what
   changes between blocking TCP, spinning TCP, two-sided RDMA, and one-sided RDMA.
2. **Where does tail latency fail first?** At what fraction of peak throughput did p99 double? At
   what fraction did p50 double? Explain why those points differ.
3. **What does the closed loop measure?** Your `-q 0` run reported a throughput below the
   capacity your sweep found, at a latency that stayed flat. Relate that throughput to the number
   of connections and the response time (Little's law), and explain why the latency of a closed
   loop cannot show a queue building. Say what a closed-loop generator tells you about a server
   and what it leaves out.
4. **What does CPU utilization miss?** Compare memcached under the CPU and memory antagonists.
   What would a scheduler conclude if it looked only at CPU utilization? Use your
   performance-counter data to explain why that conclusion can be wrong.
5. **Did isolation work?** Use your control from Part 5 to distinguish the effect of CPU
   placement from the effect of reducing antagonist parallelism. What resource contention
   appears to remain after placing the workloads on different physical cores?
6. **What would you measure next?** Name one important limitation of the conclusions you drew
   from this assignment. If this were a paper submission, what experiment would a skeptical
   reviewer reasonably ask for next?

---

## Grading: 50 points

| Component | Points |
|---|---|
| Reproducible experiment setup, raw data, and harness | 10 |
| Figures 1–3 and interpretation | 12.5 |
| Interference/isolation experiment and Figure 4 | 12.5 |
| Open-ended diagnosis | 10 |
| Short answers and experimental reasoning | 5 |

Preserve, report, and investigate unexpected results. Do not discard runs because they disagree
with your prediction. Data provenance and analysis matter more than producing a clean graph.

---

## Policies

Work individually.

Talking with classmates about CloudLab, Linux commands, build problems, or how a tool works is
encouraged. Your experimental choices, measurements, figures, and writing must be your own.

AI tools may be used. State in your report what you used them for. You must be able to
explain every result you submit and how you obtained it, in your own words and without the tool
in front of you; we will ask.

The measurements must come from machines you ran.
