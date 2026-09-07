#!/usr/bin/env bash
# CIS 7000 warm-up -- configure a node for repeatable low-latency measurement.
# Run on BOTH nodes, as root, at the start of every measurement session.
#
#   sudo ./scripts/tune.sh [iface] [rx-queues] [irq-cpus]
#   defaults:            eno1d1  4           0-3
#
# Each operation changes one mechanism and prints its before and after values.
# The script exits with an error if the kernel or NIC rejects a change. Part 1
# asks you to determine how these settings affect the measurements.
set -uo pipefail

IFACE="${1:-eno1d1}"
RXQ="${2:-4}"
IRQ_CPUS="${3:-0-3}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAIL=0

say()    { printf '[tune] %s\n' "$*"; }
step()   { printf '\n[tune] == %s ==\n' "$*"; }
before() { printf '[tune]   before: %s\n' "$*"; }
after()  { printf '[tune]   after:  %s\n' "$*"; }
warn()   { printf '[tune]   FAILED: %s\n' "$*" >&2; FAIL=1; }

[ "$(id -u)" -eq 0 ] || { echo "run me as root (sudo)" >&2; exit 1; }

# ---------------------------------------------------------------------------
step "CPU frequency governor"
# The governor decides how the CPU clock responds to load. `performance` holds
# every core at its maximum non-turbo frequency regardless of load.
b=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s x%s ", $2, $1}')
before "${b:-unavailable}"
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g"
done
a=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s x%s ", $2, $1}')
after "${a:-unavailable}"
[ "$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr -d '\n')" = "performance" ] \
    || warn "governor is not 'performance' on every CPU"

# ---------------------------------------------------------------------------
step "Turbo boost"
# With turbo enabled, a lightly loaded core may run above its rated frequency.
# no_turbo=1 disables that behavior to reduce run-to-run variation.
NT=/sys/devices/system/cpu/intel_pstate/no_turbo
if [ -r "$NT" ]; then
    before "no_turbo=$(cat "$NT")"
    echo 1 > "$NT" 2>/dev/null
    after  "no_turbo=$(cat "$NT")"
    [ "$(cat "$NT")" = "1" ] || warn "could not set no_turbo=1"
else
    before "intel_pstate not present"; after "unchanged"
fi

# ---------------------------------------------------------------------------
step "CPU idle states (PM QoS latency constraint)"
# Deeper CPU sleep states have longer exit latency. A value of 0 in
# /dev/cpu_dma_latency requests zero tolerated exit latency and keeps cores in
# the shallowest state. A background process holds the file descriptor open.
PMQOS_PID_FILE=/run/cis7000-pmqos.pid
cur_states=$(ls -d /sys/devices/system/cpu/cpu0/cpuidle/state* 2>/dev/null | wc -l)
deepest=$(for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do cat "$s/latency" 2>/dev/null; done | sort -n | tail -1)
before "cpuidle driver=$(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo none) states=$cur_states deepest_exit_latency_us=${deepest:-?} holder=$( [ -e "$PMQOS_PID_FILE" ] && kill -0 "$(cat "$PMQOS_PID_FILE")" 2>/dev/null && echo running || echo none)"
if ! { [ -e "$PMQOS_PID_FILE" ] && kill -0 "$(cat "$PMQOS_PID_FILE")" 2>/dev/null; }; then
    nohup python3 -c "
import os, struct, time
fd = os.open('/dev/cpu_dma_latency', os.O_WRONLY)
os.write(fd, struct.pack('i', 0))
while True: time.sleep(3600)
" >/dev/null 2>&1 &
    echo $! > "$PMQOS_PID_FILE"
    sleep 0.3
fi
if kill -0 "$(cat "$PMQOS_PID_FILE" 2>/dev/null)" 2>/dev/null; then
    after "holding /dev/cpu_dma_latency=0 (pid $(cat "$PMQOS_PID_FILE"))"
else
    warn "could not hold /dev/cpu_dma_latency"
fi

# ---------------------------------------------------------------------------
step "irqbalance"
# Stop irqbalance so it does not overwrite the IRQ placement configured below.
before "$(systemctl is-active irqbalance 2>/dev/null || echo absent)"
systemctl stop irqbalance 2>/dev/null || true
pkill -x irqbalance 2>/dev/null || true
after  "$(systemctl is-active irqbalance 2>/dev/null || echo absent)"

# ---------------------------------------------------------------------------
step "NIC receive interrupt moderation ($IFACE)"
# Disable adaptive interrupt moderation and set the receive delay to zero, so
# the NIC raises one interrupt per packet.
if ethtool -c "$IFACE" >/dev/null 2>&1; then
    before "$(ethtool -c "$IFACE" 2>/dev/null | awk '/Adaptive RX/{a=$3} /^rx-usecs:/{u=$2} /^rx-frames:/{f=$2} END{printf "adaptive-rx=%s rx-usecs=%s rx-frames=%s", a, u, f}')"
    ethtool -C "$IFACE" adaptive-rx off rx-usecs 0 rx-frames 1 >/dev/null 2>&1 || warn "ethtool -C refused"
    after  "$(ethtool -c "$IFACE" 2>/dev/null | awk '/Adaptive RX/{a=$3} /^rx-usecs:/{u=$2} /^rx-frames:/{f=$2} END{printf "adaptive-rx=%s rx-usecs=%s rx-frames=%s", a, u, f}')"
    [ "$(ethtool -c "$IFACE" 2>/dev/null | awk '/Adaptive RX/{print $3}')" = "off" ] || warn "adaptive-rx is still on"
else
    before "ethtool -c unsupported on $IFACE"; after "unchanged"
fi

# ---------------------------------------------------------------------------
step "NIC receive queue count ($IFACE)"
# The NIC spreads incoming flows across several receive queues, each with its
# own interrupt. This sets how many there are.
if ethtool -l "$IFACE" >/dev/null 2>&1; then
    before "rx queues=$(ethtool -l "$IFACE" 2>/dev/null | awk '/^RX:/{n=$2} END{print n}')"
    ethtool -L "$IFACE" rx "$RXQ" tx "$RXQ" >/dev/null 2>&1 \
        || ethtool -L "$IFACE" combined "$RXQ" >/dev/null 2>&1 \
        || warn "could not set queue count to $RXQ"
    after  "rx queues=$(ethtool -l "$IFACE" 2>/dev/null | awk '/^RX:/{n=$2} END{print n}')"
else
    before "ethtool -l unsupported on $IFACE"; after "unchanged"
fi

# ---------------------------------------------------------------------------
step "NIC interrupt affinity ($IFACE -> CPUs $IRQ_CPUS)"
# Each receive queue's interrupt is handled on the CPU its affinity names.
# set_irq_affinity.sh finds this interface's vectors with a traffic probe and
# assigns them one CPU each, round robin over the given list, printing the
# before/after affinity of every vector it moves.
"$HERE/set_irq_affinity.sh" "$IFACE" "$IRQ_CPUS" || warn "IRQ affinity step failed"

# ---------------------------------------------------------------------------
echo
say "kernel $(uname -r); $IFACE link $(ethtool "$IFACE" 2>/dev/null | awk '/Speed:/{print $2}')"
if [ "$FAIL" -ne 0 ]; then
    say "one or more steps FAILED; see lines marked FAILED above" >&2
    exit 1
fi
say "all steps verified"
