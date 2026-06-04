// dispatcher.cpp — binfmt_misc entry point
// Reads power state from shared memory, routes binary to QEMU or FPGA.
// Build:   cmake -B build && cmake --build build   (target: dispatcher)

#include "power_state_shm.h"
#include "qemu_executor.h"
#include "fpga_executor.h"
#include "qemu_virt_executor.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// seqlock read 
struct PowerSnapshot {
    float    temp_max_c;
    float    cpu_total_watts;
    float    fpga_watts;
    bool     throttle;
    uint64_t timestamp_us;
};

static PowerSnapshot shm_read(const PowerState* shm) {
    PowerSnapshot snap = {};
    for (;;) {
        uint32_t s1 = shm->seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;  // write in progress

        snap.temp_max_c      = shm->temp_max_c;
        snap.cpu_total_watts = shm->cpu_total_watts;
        snap.fpga_watts      = shm->fpga_watts;
        snap.throttle        = shm->throttle;
        snap.timestamp_us    = shm->timestamp_us;

        uint32_t s2 = shm->seq.load(std::memory_order_acquire);
        if (s1 == s2) break;  // consistent read
    }
    return snap;
}

// open shared memory (read-only) 

static const PowerState* open_shm_read() {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) return nullptr;

    auto* ps = static_cast<const PowerState*>(
        mmap(nullptr, SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0)
    );
    close(fd);
    return (ps == MAP_FAILED) ? nullptr : ps;
}

// routing decision 

enum class Target { QEMU, FPGA };

static Target pick_target() {
    // Manual override for testing/debugging: DISPATCHER_TARGET=qemu|fpga
    // bypasses the power-state routing.
    if (const char* force = getenv("DISPATCHER_TARGET")) {
        if (strcmp(force, "fpga") == 0) {
            fprintf(stderr, "[dispatcher] DISPATCHER_TARGET=fpga -> FPGA\n");
            return Target::FPGA;
        }
        if (strcmp(force, "qemu") == 0) {
            fprintf(stderr, "[dispatcher] DISPATCHER_TARGET=qemu -> QEMU\n");
            return Target::QEMU;
        }
        fprintf(stderr, "[dispatcher] ignoring unknown DISPATCHER_TARGET=%s\n", force);
    }

    const auto* shm = open_shm_read();
    if (!shm) return Target::QEMU;  // monitor not running, safe default

    PowerSnapshot ps = shm_read(shm);
    munmap(const_cast<PowerState*>(shm), SHM_SIZE);

    if (ps.throttle) {
        fprintf(stderr, "[dispatcher] throttle! temp=%.1fCelsius cpu=%.1fW -> FPGA\n",
                ps.temp_max_c, ps.cpu_total_watts);
        return Target::FPGA;
    }

    fprintf(stderr, "[dispatcher] temp=%.1fCelsius cpu=%.1fW -> QEMU\n",
            ps.temp_max_c, ps.cpu_total_watts);
    return Target::QEMU;
}

// Baremetal FK33/virt ELFs enter at the bootrom's jump target (0x80000000);
// anything else is a Linux user-space binary. The two classes need different
// QEMU flavours: qemu-system (QemuVirtExecutor) vs qemu-user (QemuExecutor),
// and only the baremetal class can run on the FPGA at all.
static bool is_baremetal_elf(const std::string& binary) {
    try {
        return fpgaexec::loadElf(binary).entry == fpgaexec::AddressMap{}.entry;
    } catch (const std::exception&) {
        return false;  // not a parseable RV64 executable — let qemu-user decide
    }
}

// entry point
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: dispatcher <riscv-binary> [args...]\n");
        return 1;
    }

    const std::string binary = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) args.emplace_back(argv[i]);

    const bool baremetal = is_baremetal_elf(binary);

    Target target = pick_target();
    if (target == Target::FPGA && !baremetal) {
        fprintf(stderr, "[dispatcher] %s is not a baremetal ELF, FPGA cannot run it -> QEMU\n",
                binary.c_str());
        target = Target::QEMU;
    }

    // Pick a concrete backend by power state. All backends implement the
    // same IExecutionBackend interface, so everything below is backend-agnostic
    // — which is what later lets us checkpoint() one and restore() the other
    // when the power state flips mid-run.
    std::unique_ptr<IExecutionBackend> backend;
    if (target == Target::FPGA) {
        fpgaexec::FpgaConfig cfg;
        // Optional device tree blob, DMAed to 0x88000000 before release.
        if (const char* dtb = getenv("FPGA_DTB")) cfg.dtb_path = dtb;
        backend = std::make_unique<FpgaExecutor>(std::move(cfg));

        // FpgaExecutor::launch fails fast when the card is absent or the
        // binary can't run on it — fall back to QEMU rather than refusing.
        if (!backend->launch(binary, args)) {
            fprintf(stderr, "[dispatcher] FPGA launch failed, falling back to QEMU\n");
            backend.reset();
            target = Target::QEMU;
        }
    }

    if (target == Target::QEMU) {
        // qemu-system for baremetal virt ELFs, qemu-user for Linux binaries.
        if (baremetal)
            backend = std::make_unique<QemuVirtExecutor>();
        else
            backend = std::make_unique<QemuExecutor>();

        if (!backend->launch(binary, args)) {
            fprintf(stderr, "[dispatcher] failed to launch %s\n", binary.c_str());
            return 1;
        }
    }

    return backend->wait();
}
