// dispatcher.cpp — binfmt_misc entry point
// Reads power state from shared memory, routes binary to QEMU or FPGA.
// Build:   g++ -O2 -std=c++20 dispatcher.cpp -o dispatcher -lrt

#include "power_state_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
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
    const auto* shm = open_shm_read();
    if (!shm) return Target::QEMU;  // monitor not running, safe default

    PowerSnapshot ps = shm_read(shm);
    munmap(const_cast<PowerState*>(shm), SHM_SIZE);

    if (ps.throttle) {
        fprintf(stderr, "[dispatcher] throttle! temp=%.1f°C cpu=%.1fW → FPGA\n",
                ps.temp_max_c, ps.cpu_total_watts);
        return Target::FPGA;
    }

    fprintf(stderr, "[dispatcher] temp=%.1f°C cpu=%.1fW → QEMU\n",
            ps.temp_max_c, ps.cpu_total_watts);
    return Target::QEMU;
}

// entry point
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: dispatcher <riscv-binary> [args...]\n");
        return 1;
    }

    const char* binary = argv[1];

    switch (pick_target()) {
        case Target::QEMU: {
            std::vector<const char*> args = {"qemu-riscv64-static", binary};
            for (int i = 2; i < argc; i++) args.push_back(argv[i]);
            args.push_back(nullptr);
            execvp("qemu-riscv64-static", const_cast<char* const*>(args.data()));
            perror("execvp qemu-riscv64");
            return 1;
        }

        case Target::FPGA: {
            fprintf(stderr, "[dispatcher] FPGA path not yet implemented, falling back to QEMU\n");
            std::vector<const char*> args = {"qemu-riscv64-static", binary};
            for (int i = 2; i < argc; i++) args.push_back(argv[i]);
            args.push_back(nullptr);
            execvp("qemu-riscv64-static", const_cast<char* const*>(args.data()));
            return 1;
        }
    }
}
