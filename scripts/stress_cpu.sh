#!/usr/bin/env bash
# stress_cpu.sh — load every CPU core to push package power/temperature over
# the dispatcher's throttle limits, so RISC-V binaries get routed to the FPGA.
#
#   scripts/stress_cpu.sh [seconds]   (default 120; ctrl-C stops early)
#
# Uses stress-ng (matrixprod is among the hottest methods) when installed,
# else falls back to plain busy-loops. While it runs, follow the routing
# decision live with:
#   ./build/power_monitor --watch
set -euo pipefail

DUR="${1:-120}"
NPROC="$(nproc)"

cleanup() { kill 0 2>/dev/null || true; }
trap cleanup EXIT INT TERM

echo "[stress] loading $NPROC cores for ${DUR}s"
if command -v stress-ng >/dev/null; then
    stress-ng --cpu "$NPROC" --cpu-method matrixprod --metrics-brief --timeout "${DUR}s" &
else
    echo "[stress] stress-ng not found, using busy-loop fallback (apt install stress-ng for more heat)" >&2
    for _ in $(seq "$NPROC"); do
        timeout "$DUR" sh -c 'while :; do :; done' &
    done
fi

wait
