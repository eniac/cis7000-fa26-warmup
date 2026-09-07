#!/usr/bin/env bash
# CIS 7000 warm-up -- prepare a node for the assignment.
#
#   sudo ./scripts/setup.sh              # from your clone, on BOTH nodes; about 5 min
#   sudo VERIFY_ONLY=1 ./scripts/setup.sh   # re-check a node, install nothing
#
# CloudLab boots the nodes with its stock Ubuntu 24.04 image. This script
# installs the required packages, builds mcperf, enables unprivileged access to
# hardware counters, reserves hugepages, and checks the result. It does not run
# scripts/tune.sh because Part 1 uses the stock configuration. Run `make`
# separately as your normal user to build this repository.
#
# The script is safe to rerun. Any failed check is reported as FAIL.
set -uo pipefail

SRC="${SRC:-/opt/cis7000}"          # where mcperf's source lives
VERIFY_ONLY="${VERIFY_ONLY:-0}"
FAIL=0

[ "$(id -u)" -eq 0 ] || { echo "run me as root: sudo $0" >&2; exit 1; }

T0=$(date +%s); TF=$(mktemp); echo "$T0" > "$TF"
phase() {
    local now prev; now=$(date +%s); prev=$(cat "$TF"); echo "$now" > "$TF"
    printf '[setup] %-42s %4ds (total %4ds)\n' "$1" "$((now-prev))" "$((now-T0))"
}
say()  { printf '[setup] %s\n' "$*"; }
warn() { printf '[setup] WARNING: %s\n' "$*" >&2; FAIL=1; }

if [ "$VERIFY_ONLY" = "0" ]; then

# --- packages ---------------------------------------------------------------
# NEEDRESTART_SUSPEND matters: without it needrestart restarts sshd and
# systemd-networkd on the node you are logged into, and the install hangs.
export DEBIAN_FRONTEND=noninteractive NEEDRESTART_SUSPEND=1
systemctl stop apport.service 2>/dev/null || true
systemctl mask apport.service 2>/dev/null || true

apt-get update -qq || warn "apt-get update failed"
apt-get install -y -qq --no-install-recommends \
    build-essential pkg-config git tmux htop netcat-openbsd \
    memcached stress-ng sysstat ethtool numactl msr-tools \
    linux-tools-common "linux-tools-$(uname -r)" \
    rdma-core ibverbs-utils libibverbs-dev librdmacm-dev perftest \
    libevent-dev libzmq3-dev cppzmq-dev scons gengetopt \
    python3-matplotlib python3-numpy python3-pandas \
    || warn "package install had failures"
phase "apt packages"

# The package starts memcached on 127.0.0.1:11211. Disable that instance so the
# Part 4 pidstat command finds only the assignment's memcached process.
systemctl disable --now memcached 2>/dev/null || true
phase "disable the packaged memcached unit"

# --- hugepages ------------------------------------------------------------------
# membw with 16 256 MB buffers needs 2048 hugepages. Reserve 2560 so all
# allocations can still use hugepages. Apply the setting now and at boot.
if ! grep -q '^vm.nr_hugepages' /etc/sysctl.d/99-cis7000.conf 2>/dev/null; then
    echo "vm.nr_hugepages = 2560" > /etc/sysctl.d/99-cis7000.conf
fi
sysctl -p /etc/sysctl.d/99-cis7000.conf >/dev/null 2>&1 || \
    echo 2560 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
phase "hugepages (2560 x 2MB, now and at boot)"

# --- perf for unprivileged users ---------------------------------------------
# Part 4 runs `perf stat -p <memcached pid>` without sudo. Set the paranoid
# level to -1 now and at boot so that command is permitted.
if ! grep -q '^kernel.perf_event_paranoid' /etc/sysctl.d/99-cis7000.conf 2>/dev/null; then
    echo "kernel.perf_event_paranoid = -1" >> /etc/sysctl.d/99-cis7000.conf
fi
sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || \
    echo -1 > /proc/sys/kernel/perf_event_paranoid
phase "perf_event_paranoid = -1 (perf stat -p without sudo)"

# --- load generator: mcperf -------------------------------------------------
# Use shaygalon/memcache-perf. leverich/mutilate crashes on rate-limited runs
# with the current glibc and does not report p50 or p99.9.
install -d "$SRC"
if [ ! -x "$SRC/memcache-perf/mcperf" ]; then
    rm -rf "$SRC/memcache-perf"
    git clone -q https://github.com/shaygalon/memcache-perf "$SRC/memcache-perf" \
        || warn "mcperf clone failed"
    ( cd "$SRC/memcache-perf" && make -j"$(nproc)" >/tmp/mcperf-build.log 2>&1 ) \
        || warn "mcperf build failed (see /tmp/mcperf-build.log)"
fi
[ -x "$SRC/memcache-perf/mcperf" ] || warn "mcperf binary missing after build"
install -m 0755 "$SRC/memcache-perf/mcperf" /usr/local/bin/mcperf 2>/dev/null \
    || warn "could not install mcperf"
chmod -R a+rX "$SRC"
phase "mcperf (memcache-perf)"

# --- can this tree build here? -------------------------------------------------
# Verify the build in a temporary directory. Students run `make` separately.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDCHK=$(mktemp -d)
cp -r "$HERE/rtt" "$HERE/antagonist" "$HERE/rdma" "$HERE/Makefile" "$BUILDCHK/" 2>/dev/null \
    || warn "could not copy the tree for the build check"
( cd "$BUILDCHK" && make >/tmp/warmup-buildcheck.log 2>&1 ) \
    || warn "the warm-up tools failed to build (see /tmp/warmup-buildcheck.log)"
BUILD_OK=0
[ -x "$BUILDCHK/rtt/echo_server" ] && [ -x "$BUILDCHK/rtt/echo_client" ] \
    && [ -x "$BUILDCHK/antagonist/membw" ] && [ -x "$BUILDCHK/rdma/rdma_pingpong" ] && BUILD_OK=1
export BUILD_OK
rm -rf "$BUILDCHK"
phase "build check of echo_server / echo_client / membw / rdma_pingpong"

fi   # end VERIFY_ONLY

# --- verification -----------------------------------------------------------
say "--- verification ---"
check() {  # check <label> <command...>
    if "${@:2}" >/dev/null 2>&1; then printf '[setup]   ok    %s\n' "$1"
    else printf '[setup]   FAIL  %s\n' "$1"; FAIL=1; fi
}
check "memcached present"          command -v memcached
check "memcached unit disabled"    bash -c '! systemctl is-enabled memcached 2>/dev/null | grep -q "^enabled$"'
check "mcperf present"             command -v mcperf
check "mcperf rate limiting (-q)"  bash -c 'mcperf --help 2>&1 | grep -q qps'
check "stress-ng present"          command -v stress-ng
check "stress-ng ackermann method" bash -c 'cd /tmp && stress-ng --cpu 1 --cpu-method ackermann --timeout 1s'
check "pidstat present"            command -v pidstat
check "mpstat present"             command -v mpstat
check "nc present"                 command -v nc
check "ethtool present"            command -v ethtool
check "ibverbs userspace"          command -v ibv_devinfo
check "RDMA device visible"        bash -c 'ibv_devinfo -l | grep -qv "^0 "'
check "perftest present"           command -v ib_read_lat
check "warm-up tools build here"   bash -c '[ "${BUILD_OK:-1}" = 1 ]'
check "tmux present"               command -v tmux
check "htop present"               command -v htop
check "perf present"               command -v perf
check "perf usable unprivileged"   bash -c '[ "$(cat /proc/sys/kernel/perf_event_paranoid)" -le 1 ]'
check "perf counts cycles"         bash -c 'perf stat -e cycles,instructions -x, true 2>&1 | grep -q cycles'
check "hugepages reserved"         bash -c '[ "$(cat /proc/sys/vm/nr_hugepages)" -ge 2560 ]'
check "matplotlib importable"      python3 -c "import matplotlib, numpy"

if [ "$FAIL" -ne 0 ]; then
    echo "[setup] FAILED -- fix the lines marked FAIL before measuring anything." >&2
    exit 1
fi
say "all checks passed. Next: make, then scripts/check-system.sh (handout Part 1)."
