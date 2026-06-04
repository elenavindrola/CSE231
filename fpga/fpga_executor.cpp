// fpga_executor.cpp — see fpga_executor.h.

#include "fpga_executor.h"

#include <unistd.h>

#include <cstdio>

bool FpgaExecutor::precheck(const std::string& binary) {
    // 1. The ELF must parse as RV64 and enter at the bootrom's jump target.
    try {
        fpgaexec::ElfImage img = fpgaexec::loadElf(binary);
        if (img.entry != config_.map.entry) {
            fprintf(stderr,
                    "[fpga] %s: entry 0x%llx != bootrom target 0x%llx "
                    "(not a baremetal FK33/virt ELF)\n",
                    binary.c_str(), (unsigned long long)img.entry,
                    (unsigned long long)config_.map.entry);
            return false;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[fpga] %s: %s\n", binary.c_str(), e.what());
        return false;
    }

    // 2. The XDMA char devices must exist and be accessible.
    if (access(config_.h2c.c_str(), R_OK | W_OK) != 0 ||
        access(config_.c2h.c_str(), R_OK | W_OK) != 0) {
        fprintf(stderr, "[fpga] XDMA devices unavailable (%s, %s) — card absent "
                "or missing permissions (need root)\n",
                config_.h2c.c_str(), config_.c2h.c_str());
        return false;
    }
    return true;
}

std::unique_ptr<fpgaexec::Backend> FpgaExecutor::make_backend() {
    return std::make_unique<fpgaexec::FpgaBackend>(config_);
}
