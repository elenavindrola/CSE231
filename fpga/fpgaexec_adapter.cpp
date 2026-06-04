// fpgaexec_adapter.cpp — see fpgaexec_adapter.h.

#include "fpgaexec_adapter.h"

#include <cstdio>

FpgaexecAdapter::~FpgaexecAdapter() {
    if (worker_.joinable()) worker_.join();
}

bool FpgaexecAdapter::launch(const std::string& binary,
                             const std::vector<std::string>& args) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != ExecutionState::Idle) return false;
    }

    // Baremetal guests get no argv; refusing here would block legitimate
    // binfmt_misc invocations that always pass argv through, so just warn.
    if (!args.empty())
        fprintf(stderr, "[fpgaexec] ignoring %zu argument(s): baremetal guest has no argv\n",
                args.size());

    if (!precheck(binary)) return false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = ExecutionState::Running;
    }
    worker_ = std::thread([this, binary] {
        std::unique_ptr<fpgaexec::Backend> backend = make_backend();
        fpgaexec::RunResult res = backend->run(binary);

        std::lock_guard<std::mutex> lk(mu_);
        result_ = std::move(res);
        state_  = result_.error.empty() ? ExecutionState::Exited
                                        : ExecutionState::Error;
    });
    return true;
}

ExecutionState FpgaexecAdapter::state() {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

int FpgaexecAdapter::wait() {
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == ExecutionState::Idle) return -1;  // never launched

    // Relay the guest's console (UART capture / QEMU output) like a local
    // process would.
    if (!result_.console.empty()) {
        fwrite(result_.console.data(), 1, result_.console.size(), stdout);
        if (result_.console.back() != '\n') fputc('\n', stdout);
        fflush(stdout);
    }

    if (!result_.error.empty()) {
        fprintf(stderr, "[fpgaexec] run failed: %s\n", result_.error.c_str());
        return 2;
    }
    if (result_.passed) return 0;
    // Guest FAIL: propagate its code (clamped into exit-code range).
    return result_.code ? static_cast<int>(result_.code & 0xff) : 1;
}

bool FpgaexecAdapter::stop() {
    return false;  // not preemptible yet — see header
}

std::optional<ExecutionCheckpoint> FpgaexecAdapter::checkpoint() {
    return std::nullopt;  // M3
}

bool FpgaexecAdapter::restore(const ExecutionCheckpoint&, const std::string&,
                              const std::vector<std::string>&) {
    return false;  // M3
}
