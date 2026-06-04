#!/usr/bin/env bash
# run_riscv.sh — run a RISC-V ELF through the power-aware dispatcher.
#
#   scripts/run_riscv.sh <riscv-elf> [args...]
#
# Ensures the power monitor daemon is running (starts it via sudo if not),
# then invokes the dispatcher, which routes to QEMU or the FPGA by power state.
# Environment passthrough:
#   DISPATCHER_TARGET=qemu|fpga   force a backend (testing)
#   FPGA_DTB=<path>               DTB to load at 0x88000000
#   QEMU_SYSTEM=<path>            qemu-system-riscv64 (or wrapper) override
#   MONITOR_ARGS="..."            extra power_monitor flags, e.g.
#                                 MONITOR_ARGS="--power-limit 40" for a demo
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISPATCHER="$REPO/build/dispatcher"
MONITOR="$REPO/build/power_monitor"
SHM=/dev/shm/cpu_power_state

if [ $# -lt 1 ]; then
    echo "usage: $(basename "$0") <riscv-elf> [args...]" >&2
    exit 1
fi
for bin in "$DISPATCHER" "$MONITOR"; do
    [ -x "$bin" ] || { echo "missing $bin — build first: cmake -B build && cmake --build build -j" >&2; exit 1; }
done

# Start the power monitor if it isn't running (RAPL needs root). A leftover
# shm segment from a dead monitor would freeze the routing decision, so
# recreate it.
if ! pgrep -x power_monitor >/dev/null; then
    echo "[run_riscv] starting power_monitor (sudo)" >&2
    sudo rm -f "$SHM"
    # shellcheck disable=SC2086  # MONITOR_ARGS is intentionally word-split
    sudo "$MONITOR" ${MONITOR_ARGS:-} &
    sleep 1  # let RAPL prime and the first sample land
fi

# The XDMA char devices are root-owned; elevate only when the FPGA is present
# and we can't already write to it.
SUDO=()
if [ -e /dev/xdma0_h2c_0 ] && [ ! -w /dev/xdma0_h2c_0 ]; then
    SUDO=(sudo --preserve-env=DISPATCHER_TARGET,FPGA_DTB,QEMU_SYSTEM)
fi

exec "${SUDO[@]}" "$DISPATCHER" "$@"
