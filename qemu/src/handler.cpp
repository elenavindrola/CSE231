#include "qemu_executor.h"

#include <cstdio>

// QEMU-only smoke harness.
//
// SUPERSEDED as the binfmt_misc entry point: the top-level dispatcher
// (../../dispatcher.cpp) is now the registered interpreter — it reads power
// state and routes to a backend (QEMU vs FPGA) via IExecutionBackend.
//
// This remains as a minimal harness that runs a binary unconditionally under
// QEMU with no routing, useful for isolating QemuExecutor behaviour.
// Invoke as: riscv-handler <riscv-binary> [args...]

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <riscv-binary> [args...]\n", argv[0]);
        return 1;
    }

    QemuConfig config;
    config.enable_gdb = false;

    QemuExecutor qemu(config);

    std::vector<std::string> args;
    for (int i = 2; i < argc; i++)
        args.emplace_back(argv[i]);

    if (!qemu.launch(argv[1], args)) {
        fprintf(stderr, "Failed to launch %s under QEMU\n", argv[1]);
        return 1;
    }

    return qemu.wait();
}
