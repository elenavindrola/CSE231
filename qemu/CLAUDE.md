# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

To disable `/proc/pid/mem` memory transfer and fall back to the GDB stub:
```bash
cmake -B build -DUSE_PROC_MEM=OFF
cmake --build build -j$(nproc)
```

## Tests

Unit tests (no external dependencies):
```bash
./build/unit_tests                    # context serialization + GDB client (mock server)
```

Integration tests (require `qemu-riscv64` in PATH and the pre-built RISC-V test binaries in `tests/`):
```bash
./build/integration_tests             # QemuExecutor via GDB stub
./build/integration_tests_procmem     # QemuExecutor via /proc/pid/mem
./build/proc_mem_tests                # ProcMem against native + QEMU processes
```

Run all via CTest:
```bash
cd build && ctest --output-on-failure
```

Run a single GTest case:
```bash
./build/unit_tests --gtest_filter='GdbClientTest.ReadRegisters'
```

Integration tests auto-skip when `qemu-riscv64` is missing or test binaries aren't found. Test binaries can be overridden with env vars: `RISCV_TEST_BIN`, `RISCV_SPIN_BIN`, `RISCV_PATTERN_BIN`.

## RISC-V test binaries

`tests/hello_rv`, `tests/spin_rv`, `tests/mempattern_rv` are pre-built statically-linked RV64 ELFs compiled from the `.c` files alongside them. Rebuild with a RISC-V cross-compiler:
```bash
riscv64-linux-gnu-gcc -static -o tests/hello_rv tests/hello_rv.c
```

## Development guidance

- Prefer the `/proc/pid/mem` path (ProcMem) for all memory operations. Do not work on or optimize the GDB stub path unless explicitly asked.

## Architecture

This is the QEMU emulation path of a hybrid RISC-V scheduler that routes execution between QEMU (software emulation) and an FPGA backend. The repo-root **`dispatcher`** is the Linux `binfmt_misc` interpreter — the kernel invokes it whenever a RISC-V ELF is executed on the host. It reads power state from shared memory (`../power_state_shm.h`) and routes the guest to a concrete `IExecutionBackend` (`QemuExecutor` today, `FpgaExecutor` later). Build it via the top-level `../CMakeLists.txt`, which pulls in this directory's `sched_core` library.

### Execution flow

1. **`dispatcher.cpp`** (repo root, binfmt_misc entry point) — reads power state, picks a backend, and drives it through `IExecutionBackend::launch`/`wait`. **`handler.cpp`** here is superseded as the entry point; it remains only as a QEMU-only smoke harness that runs a binary unconditionally under `QemuExecutor` with no routing.

2. **`IExecutionBackend`** (`execution_backend.h`) — abstract interface for any execution backend. Defines `launch`, `stop`, `checkpoint`, `restore`, and `wait`. The FPGA backend will implement this same interface.

3. **`QemuExecutor`** — forks `qemu-riscv64` as a child process. Optionally attaches a GDB stub (`-g` flag) for register/memory inspection. Two memory transfer strategies controlled at compile time:
   - Default: GDB RSP `m`/`M` packets via `GdbClient`
   - `USE_PROC_MEM`: direct reads/writes through `/proc/pid/mem` via `ProcMem`

4. **`GdbClient`** — speaks the GDB Remote Serial Protocol over TCP to QEMU's built-in GDB stub. Handles register read/write (`g`/`G` packets), memory read/write (`m`/`M`), continue (`c`), interrupt (0x03), and detach (`D`). Register layout is RISC-V 64-bit: 32 GPRs + PC + 32 FPRs + FCSR, all little-endian.

5. **`ProcMem`** — reads/writes QEMU's host-side process memory via `/proc/pid/mem` and parses `/proc/pid/maps` for region discovery. This is faster than GDB for bulk memory transfer but operates on host virtual addresses (QEMU's address space), not guest addresses.

6. **`RiscvContext`** — serializable snapshot of RISC-V register state (pc, 32 GPRs, 32 FPRs, fcsr). Binary format with `RVCTX100` magic header. Used for checkpoint/restore across backends.

7. **`binfmt`** — registers/unregisters the handler with `/proc/sys/fs/binfmt_misc` by matching RISC-V 64-bit ELF magic bytes.

### binfmt_misc registration

```bash
sudo scripts/register_binfmt.sh [path-to-interpreter]
```
Defaults to the dispatcher at `../build/dispatcher` (repo-root build dir). Pass an explicit path to register the QEMU-only `build/qemu/riscv-handler` smoke harness instead. Requires root for `/proc/sys/fs/binfmt_misc/register`.
