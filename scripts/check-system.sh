#!/usr/bin/env bash
# CIS 7000 warm-up: report system settings relevant to latency measurements.
# This script is read-only. Save its output before and after running tune.sh.
#
#   ./scripts/check-system.sh [iface]        (default eno1d1; sudo not required)
set -uo pipefail
IFACE="${1:-eno1d1}"

hdr() { printf '\n## %s\n' "$*"; }
kv()  { printf '%-34s %s\n' "$1" "$2"; }
rd()  { cat "$1" 2>/dev/null || echo "n/a"; }

echo "# check-system  $(date -Is)  host=$(hostname)"

hdr "OS"
kv "kernel"            "$(uname -r)"
kv "distribution"      "$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-?}")"

hdr "CPU"
kv "model"             "$(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); print $2}')"
kv "sockets/cores/threads" "$(lscpu | awk -F: '/^Socket\(s\)/{s=$2} /^Core\(s\) per socket/{c=$2} /^Thread\(s\) per core/{t=$2} END{gsub(/ /,"",s);gsub(/ /,"",c);gsub(/ /,"",t); print s" / "c" / "t}')"
kv "online CPUs"       "$(rd /sys/devices/system/cpu/online)"
echo "thread siblings (logical CPU -> its siblings):"
for c in /sys/devices/system/cpu/cpu[0-9]*; do
    n=${c##*/cpu}
    printf '  cpu%-3s %s\n' "$n" "$(rd "$c/topology/thread_siblings_list")"
done | sort -k1.4n
kv "scaling driver"    "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)"
kv "governor (per CPU, counted)" "$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s x%s ", $2, $1}')"
kv "cpu0 current freq (kHz)" "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
kv "cpu0 min/max freq (kHz)" "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq) / $(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq)"
kv "intel_pstate no_turbo" "$(rd /sys/devices/system/cpu/intel_pstate/no_turbo)"
kv "cpuidle driver"    "$(rd /sys/devices/system/cpu/cpuidle/current_driver)"
echo "cpu0 idle states (name / exit latency us / disabled):"
for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
    [ -d "$s" ] || continue
    printf '  %-10s %6s  %s\n' "$(rd "$s/name")" "$(rd "$s/latency")" "$(rd "$s/disable")"
done
# The holder runs as root, so an unprivileged `kill -0` returns EPERM; /proc
# is readable by everyone, so check there.
pmq_pid=$(cat /run/cis7000-pmqos.pid 2>/dev/null)
if [ -n "$pmq_pid" ] && [ -d "/proc/$pmq_pid" ] && tr '\0' ' ' < "/proc/$pmq_pid/cmdline" 2>/dev/null | grep -q cpu_dma_latency; then
    kv "pm_qos holder (cis7000)" "running pid $pmq_pid"
else
    kv "pm_qos holder (cis7000)" "none"
fi
# Distinguish a stopped irqbalance service from a missing service.
if systemctl cat irqbalance.service >/dev/null 2>&1; then
    kv "irqbalance"    "$(systemctl is-active irqbalance 2>/dev/null)"
else
    kv "irqbalance"    "absent"
fi

hdr "NIC $IFACE"
kv "link"              "$(ethtool "$IFACE" 2>/dev/null | awk '/Link detected/{print $3}')"
kv "speed"             "$(ethtool "$IFACE" 2>/dev/null | awk '/Speed:/{print $2}')"
kv "driver"            "$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}')"
kv "pci bus"           "$(ethtool -i "$IFACE" 2>/dev/null | awk '/bus-info/{print $2}')"
kv "addresses"         "$(ip -br addr show "$IFACE" 2>/dev/null | awk '{$1=$2=""; print}' | xargs)"
echo "interrupt coalescing:"
ethtool -c "$IFACE" 2>/dev/null | grep -E 'Adaptive RX|^rx-usecs:|^rx-frames:|^tx-usecs:' | sed 's/^/  /'
echo "queues (current / max):"
ethtool -l "$IFACE" 2>/dev/null | awk '/^Pre-set/{m=1} /^Current/{m=2} /^RX:|^TX:|^Combined:/{ if(m==1) mx[$1]=$2; else cur[$1]=$2 } END{for(k in cur) printf "  %-10s %s / %s\n", k, cur[k], mx[k]}'
echo "interrupt vectors for this device and their CPU affinity:"
bus=$(ethtool -i "$IFACE" 2>/dev/null | awk '/bus-info/{print $2}')
found=0
while read -r line; do
    irq=${line%%:*}; irq=${irq// /}
    aff=$(rd "/proc/irq/$irq/smp_affinity_list")
    name=$(printf '%s' "$line" | awk '{print $NF}')
    printf '  irq %-5s cpus=%-8s %s\n' "$irq" "$aff" "$name"; found=1
done < <(grep -E "mlx|${bus:-NOBUS}" /proc/interrupts 2>/dev/null)
[ "$found" -eq 1 ] || echo "  (none matched; look for the device in /proc/interrupts by hand)"

hdr "RDMA"
rdma link show 2>/dev/null | sed 's/^/  /' || echo "  rdma tool not available"
echo "devices (ibv_devinfo -l):"
ibv_devinfo -l 2>/dev/null | sed 's/^/  /'

hdr "memory"
kv "MemTotal"          "$(awk '/MemTotal/{print $2" kB"}' /proc/meminfo)"
kv "HugePages_Total"   "$(awk '/HugePages_Total/{print $2}' /proc/meminfo)"
kv "perf_event_paranoid" "$(rd /proc/sys/kernel/perf_event_paranoid)"
