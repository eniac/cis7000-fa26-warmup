#!/usr/bin/env bash
# Pin the experiment NIC's receive-queue interrupts to a set of CPUs.
#
#   sudo ./scripts/set_irq_affinity.sh [iface] [cpu-list] [peer-ip]
#   sudo ./scripts/set_irq_affinity.sh eno1d1 0-3 10.10.1.1
#
# Vector discovery
#
# The Mellanox affinity scripts search /proc/interrupts for the interface name.
# That does not work here because mlx4 names vectors after the PCI device:
#
#   51: mlx4-async@pci:0000:09:00.0
#   52: mlx4-1@0000:09:00.0
#   ...  33 vectors in total
#
# On the m510, eno1 and eno1d1 are two ports of the same ConnectX-3. All 33
# vectors belong to one device, and the names do not identify the port.
#
# Identify the experiment port's vectors by sampling counters before and after
# a traffic burst on that interface. If the peer is unavailable, pin all mlx4
# vectors. This fallback also moves control-network interrupts, so avoid
# streaming output over SSH during measurements.
set -uo pipefail

IFACE="${1:-eno1d1}"
CPUS="${2:-0-3}"
PEER="${3:-}"

[ "$(id -u)" -eq 0 ] || { echo "run me as root" >&2; exit 1; }

# Guess the peer as .1/.2 on this interface's /24 if not supplied.
if [ -z "$PEER" ]; then
    myip=$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
    case "$myip" in
        *.1) PEER="${myip%.1}.2" ;;
        *.2) PEER="${myip%.2}.1" ;;
    esac
fi

irq_nums() { awk -F: '/mlx4-[0-9]+@/{gsub(/ /,"",$1); print $1}' /proc/interrupts; }
counts_for() { awk -v want="$1" -F: '/mlx4-[0-9]+@/{gsub(/ /,"",$1);
    if ($1==want) { s=0; n=split($2,f," "); for(i=1;i<=n;i++) if (f[i] ~ /^[0-9]+$/) s+=f[i]; print s } }' /proc/interrupts; }

ALL=$(irq_nums)
[ -n "$ALL" ] || { echo "[irq] no mlx4 vectors found in /proc/interrupts" >&2; exit 1; }

# Wait for the link. `ethtool -L` (queue-count change) resets the NIC, so a
# probe fired immediately after it races link renegotiation and always fails.
reachable=0
if [ -n "$PEER" ]; then
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if ping -c1 -W1 -I "$IFACE" "$PEER" >/dev/null 2>&1; then reachable=1; break; fi
        sleep 1
    done
fi

SELECTED=""
if [ "$reachable" -eq 1 ]; then
    # Control traffic on the other port may increment its vectors during the
    # probe. Measure a quiet baseline and require a larger probe delta.
    declare -A quiet before
    for i in $ALL; do quiet[$i]=$(counts_for "$i"); done
    sleep 2
    for i in $ALL; do before[$i]=$(counts_for "$i"); done

    # Probe with many TCP flows. RSS hashes ICMP using the IP pair, so a ping
    # flood reaches only one queue. Distinct TCP source ports exercise the other
    # receive queues.
    for _ in $(seq 1 40); do
        timeout 0.3 bash -c "exec 3<>/dev/tcp/$PEER/22" >/dev/null 2>&1
    done
    ping -f -q -c 2000 -I "$IFACE" "$PEER" >/dev/null 2>&1

    for i in $ALL; do
        a=$(counts_for "$i"); a=${a:-0}
        b=${before[$i]:-0}; q=${quiet[$i]:-0}
        bg=$(( b - q ))                    # interrupts in the 2 s quiet window
        # Selected if the probe moved it by more than 20 and by more than ten
        # times what the quiet window did.
        if [ "$(( a - b ))" -gt 20 ] && [ "$(( a - b ))" -gt "$(( 10 * bg ))" ]; then SELECTED="$SELECTED $i"; fi
    done
    if [ -n "$SELECTED" ]; then
        echo "[irq] detected $IFACE vectors via traffic probe to $PEER:$(echo "$SELECTED" | tr '\n' ' ')"
    fi
fi

if [ -z "$SELECTED" ]; then
    SELECTED="$ALL"
    echo "[irq] could not isolate $IFACE's vectors (no reachable peer);" \
         "pinning ALL mlx4 vectors as a safe superset"
fi

# Expand a list such as "0-3,6" and assign one CPU to each vector, round-robin.
# A multi-CPU mask can place every vector on the lowest CPU in the mask.
cpu_list=()
for part in ${CPUS//,/ }; do
    case "$part" in
        *-*) for c in $(seq "${part%-*}" "${part#*-}"); do cpu_list+=("$c"); done ;;
        *)   cpu_list+=("$part") ;;
    esac
done
ncpu=${#cpu_list[@]}
[ "$ncpu" -gt 0 ] || { echo "[irq] bad cpu list: $CPUS" >&2; exit 1; }

# Print each vector's affinity before and after the change.
n=0; k=0; bad=0
for i in $SELECTED; do
    target=${cpu_list[$(( k % ncpu ))]}; k=$((k+1))
    was=$(cat "/proc/irq/$i/smp_affinity_list" 2>/dev/null || echo '?')
    if [ -w "/proc/irq/$i/smp_affinity_list" ]; then
        echo "$target" > "/proc/irq/$i/smp_affinity_list" 2>/dev/null && n=$((n+1))
    fi
    now=$(cat "/proc/irq/$i/smp_affinity_list" 2>/dev/null || echo '?')
    printf '[irq]   irq %-5s affinity %-8s -> %s\n' "$i" "$was" "$now"
    # Verify: some vectors are kernel-managed and refuse the write.
    [ "$now" = "$target" ] || bad=$((bad+1))
done
echo "[irq] $n vector(s) assigned one CPU each, round robin over $CPUS"
[ "$bad" -eq 0 ] || echo "[irq] WARNING: $bad vector(s) did not accept the affinity write" >&2

echo "[irq] verify under load with: watch -d 'grep mlx4 /proc/interrupts'"
