#!/usr/bin/env bash
set -euo pipefail

# The dispatcher (built by the top-level CMake at repo-root build/) is the
# binfmt_misc entry point: it reads power state and routes to a backend.
# Pass an explicit path to register a different interpreter (e.g. the
# QEMU-only riscv-handler smoke harness at build/qemu/riscv-handler).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HANDLER="${1:-${SCRIPT_DIR}/../../build/dispatcher}"

if [[ ! -x "$HANDLER" ]]; then
    echo "Handler not found: $HANDLER"
    echo "Build the project first (cmake --build build)"
    exit 1
fi

HANDLER="$(realpath "$HANDLER")"

if [[ ! -d /proc/sys/fs/binfmt_misc ]]; then
    echo "Mounting binfmt_misc..."
    sudo mount -t binfmt_misc none /proc/sys/fs/binfmt_misc
fi

if [[ -f /proc/sys/fs/binfmt_misc/riscv64-sched ]]; then
    echo "Removing existing registration..."
    echo -1 | sudo tee /proc/sys/fs/binfmt_misc/riscv64-sched >/dev/null
fi

echo "Registering handler: $HANDLER"
echo ":riscv64-sched:M:0:\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xf3\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:${HANDLER}:" \
    | sudo tee /proc/sys/fs/binfmt_misc/register >/dev/null

if [[ -f /proc/sys/fs/binfmt_misc/riscv64-sched ]]; then
    echo "Registered successfully:"
    cat /proc/sys/fs/binfmt_misc/riscv64-sched
else
    echo "Registration failed"
    exit 1
fi
