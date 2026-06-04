// fpga_executor.cpp — see fpga_executor.h.

#include "fpga_executor.h"

#include <unistd.h>

#include <cstdio>

FpgaExecutor::FpgaExecutor(fpgaexec::FpgaConfig config)
    : config_(std::move(config)) {}

FpgaExecutor::~FpgaExecutor() {
    if (worker_.joinable()) worker_.join();
}

void FpgaExecutor::set_state(ExecutionState s) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = s;
}

bool FpgaExecutor::launch(const std::string& binary,
                          const std::vector<std::string>& args) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != ExecutionState::Idle) return false;
    }

    // Baremetal guests get no argv; refusing here would block legitimate
    // binfmt_misc invocations that always pass argv through, so just warn.
    if (!args.empty())
        fprintf(stderr, "[fpga] ignoring %zu argument(s): baremetal guest has no argv\n",
                args.size());

    // Fail fast (cheap, host-side only) so the dispatcher can fall back to
    // QEMU instead of burning the run timeout against a card that can't work:
    //  1. the ELF must parse as RV64 and enter at the bootrom's jump target;
    //  2. the XDMA char devices must exist and be accessible.
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

    if (access(config_.h2c.c_str(), R_OK | W_OK) != 0 ||
        access(config_.c2h.c_str(), R_OK | W_OK) != 0) {
        fprintf(stderr, "[fpga] XDMA devices unavailable (%s, %s) — card absent "
                "or missing permissions (need root)\n",
                config_.h2c.c_str(), config_.c2h.c_str());
        return false;
    }

    set_state(ExecutionState::Running);
    worker_ = std::thread([this, binary] {
        fpgaexec::FpgaBackend backend(config_);
        fpgaexec::RunResult res = backend.run(binary);

        std::lock_guard<std::mutex> lk(mu_);
        result_ = std::move(res);
        state_  = result_.error.empty() ? ExecutionState::Exited
                                        : ExecutionState::Error;
    });
    return true;
}

ExecutionState FpgaExecutor::state() {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

int FpgaExecutor::wait() {
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == ExecutionState::Idle) return -1;  // never launched

    // Relay the guest's console (captured UART TX) like a local process would.
    if (!result_.console.empty()) {
        fwrite(result_.console.data(), 1, result_.console.size(), stdout);
        if (result_.console.back() != '\n') fputc('\n', stdout);
        fflush(stdout);
    }

    if (!result_.error.empty()) {
        fprintf(stderr, "[fpga] run failed: %s\n", result_.error.c_str());
        return 2;
    }
    if (result_.passed) return 0;
    // Finisher FAIL: propagate the guest's code (clamped into exit-code range).
    return result_.code ? static_cast<int>(result_.code & 0xff) : 1;
}

bool FpgaExecutor::stop() {
    return false;  // not preemptible yet — see header
}

std::optional<ExecutionCheckpoint> FpgaExecutor::checkpoint() {
    return std::nullopt;  // M3
}

bool FpgaExecutor::restore(const ExecutionCheckpoint&, const std::string&,
                           const std::vector<std::string>&) {
    return false;  // M3
}
