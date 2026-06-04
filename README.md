# Power and Temperature Aware Dispatcher for Load Balancing QEMU and FPGA

Routes RISC-V binaries to QEMU or FPGA based on CPU power and thermal state.

## Build

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Targets: `dispatcher` (binfmt_misc entry point), `power_monitor` (daemon),
plus the `qemu/` test suite (`cd build && ctest`).

## Run

One command — starts the power monitor if needed, then dispatches:

```bash
scripts/run_riscv.sh <riscv-elf> [args...]
# example: the CVA6 arithmetic-validation kernel (baremetal, runs on FPGA or QEMU virt)
scripts/run_riscv.sh cva6_fk33/sw/examples/arith/arith.elf
```

Or manually:

```bash
# 1. start the power monitor (requires sudo for RAPL access)
sudo rm -f /dev/shm/cpu_power_state
sudo ./build/power_monitor &
sleep 1   # give sampling time

# 2. run a RISC-V binary through the dispatcher
./build/dispatcher <riscv-binary>
```

The dispatcher prints which backend was chosen, for example:
```
[dispatcher] temp=63.0 Celsius cpu=2.2W -> QEMU
```

Useful environment variables (both `run_riscv.sh` and `dispatcher`):

| Variable | Effect |
|-|-|
| `DISPATCHER_TARGET=qemu\|fpga` | force a backend, bypassing power state |
| `FPGA_DTB=<path>` | DTB to DMA to `0x88000000` before release |
| `QEMU_SYSTEM=<path>` | qemu-system-riscv64 (or wrapper) override |
| `MONITOR_ARGS="--power-limit 40"` | extra flags for the auto-started monitor (`run_riscv.sh` only) |

## Routing logic

| Condition | Backend |
|-|-|
| T < 85 Celsius and cpu < 80W | QEMU |
| T > 85 Celsius or cpu > 80W | FPGA |
| monitor not running | QEMU (safe default) |
| binary is not a baremetal virt ELF (entry != `0x80000000`) | QEMU (FPGA cannot run it) |
| FPGA launch fails (card absent / no permissions) | QEMU fallback |

Baremetal virt ELFs go to `qemu-system-riscv64 -machine virt` (same device map
as the FK33 bitstream); Linux user-space binaries go to `qemu-riscv64`
(user-mode). Limits are tunable: `power_monitor --temp-limit C --power-limit W`.

## Writing RISC-V binaries for the FPGA path (BSP requirement)

A binary can only run on the FPGA (and on `qemu-system -machine virt`) if it is
**baremetal and built against the BSP** in `cva6_fk33/sw/bsp/` — a plain
Linux-targeted RISC-V binary will always route to user-mode QEMU instead. The
BSP contract:

- `#include "bsp.h"` — gives you `printf`/`uart_puts` (16550 UART0 @
  `0x10000000`) and `_exit` via the SiFive finisher (`return 0` = PASS,
  nonzero = FAIL with that code as the host exit code)
- entry point is `int main(int hartid, void *dtb)`; the image **must** enter at
  `0x80000000` (the bootrom's hardcoded jump target) — `bsp/virt.ld` +
  `bsp/crt0.S` arrange this
- compile **RV64IMAC only** (`-march=rv64imac_zicsr -mabi=lp64`): CVA6 on the
  card has **no FPU**, a stray FP instruction traps/hangs the core. QEMU virt
  is a superset, so the same binary still runs there as the golden reference
- freestanding flags: `-nostdlib -nostartfiles -fno-builtin -ffreestanding
  -mcmodel=medany -Wl,--build-id=none` (see `cva6_fk33/sw/Makefile`)

Easiest flow — drop `examples/<name>/main.c` next to the others and add a rule
in `cva6_fk33/sw/Makefile` (copy the `fib` one), then:

```bash
make -C cva6_fk33/sw examples/<name>/<name>.elf   # needs riscv64-unknown-elf- on PATH
scripts/run_riscv.sh cva6_fk33/sw/examples/<name>/<name>.elf
```

Ready-made sanity kernels (all self-checking, PASS/FAIL via the finisher):
`arith` (ALU/mul/div/atomics differential check), `fib` (recursion + stack),
`memtest` (1 MiB DRAM/HBM window at +8 MiB), `sort` (heapsort, loads/stores +
branches), `hello`.

## Demo: load the CPU until jobs migrate to the FPGA

```bash
# terminal 1: watch the routing decision flip (no root needed)
./build/power_monitor --watch

# terminal 2: peg every core (stress-ng if installed, else busy-loops)
scripts/stress_cpu.sh 180

# terminal 3: once --watch shows "-> FPGA", dispatch
scripts/run_riscv.sh cva6_fk33/sw/examples/arith/arith.elf
```

If the machine can't reach the default limits (e.g. a well-cooled 65W-TDP
part), lower them when starting the monitor:

```bash
sudo pkill power_monitor; sudo rm -f /dev/shm/cpu_power_state
MONITOR_ARGS="--power-limit 40 --temp-limit 70" scripts/run_riscv.sh <elf>
```

## Dependencies

```bash
sudo apt-get install -y g++ cmake qemu-system-misc qemu-user gcc-riscv64-unknown-elf stress-ng
```

## Notes

- The power monitor reads RAPL via `/sys/class/powercap/` (root required);
  works on Intel and AMD Zen (same powercap interface)
- CPU temperature comes from `/sys/class/thermal/` zones **and** hwmon
  (`k10temp`/`coretemp`) — AMD desktop parts only expose the latter
- Power state is shared via POSIX shared memory at `/dev/shm/cpu_power_state`
  (seqlock); follow it live with `./build/power_monitor --watch`
- The FPGA backend needs the FK33 card, the XDMA driver (`/dev/xdma0_*`) and
  root; see `cva6_fk33/README.md`
