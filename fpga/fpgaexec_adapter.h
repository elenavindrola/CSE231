// fpgaexec_adapter.h — IExecutionBackend over a synchronous fpgaexec::Backend.
//
// libfpgaexec backends (FpgaBackend, QemuBackend) are one-shot: run(elf) blocks
// until the finisher fires / the process exits and returns a RunResult. This
// base adapts that to the dispatcher's async launch/wait interface by running
// it on a worker thread. Concrete executors supply:
//   - precheck():     cheap host-side validation so launch() fails fast and the
//                     dispatcher can fall back instead of burning the run timeout;
//   - make_backend(): the fpgaexec::Backend to run.

#pragma once

#include "execution_backend.h"
#include "fpgaexec/fpgaexec.hpp"

#include <memory>
#include <mutex>
#include <thread>

class FpgaexecAdapter : public IExecutionBackend {
public:
    ~FpgaexecAdapter() override;

    FpgaexecAdapter(const FpgaexecAdapter&) = delete;
    FpgaexecAdapter& operator=(const FpgaexecAdapter&) = delete;

    bool launch(const std::string& binary,
                const std::vector<std::string>& args) override;

    ExecutionState state() override;

    // Joins the worker, prints the guest's console (UART capture / QEMU
    // stdout) to stdout and returns an exit code: 0 PASS, the guest's FAIL
    // code (or 1) on FAIL, 2 host error.
    int wait() override;

    // A run in flight cannot be preempted yet: Backend::run() owns the card /
    // QEMU process until completion or timeout. M3 (migration) work.
    bool stop() override;

    // Baremetal-side snapshot/restore is future M3 work (read registers/memory
    // back over XDMA — see fpgaexec::Xdma).
    std::optional<ExecutionCheckpoint> checkpoint() override;
    bool restore(const ExecutionCheckpoint& ckpt,
                 const std::string& binary,
                 const std::vector<std::string>& args) override;

    // Result of the completed run (valid after wait()).
    const fpgaexec::RunResult& result() const { return result_; }

protected:
    FpgaexecAdapter() = default;

    // Validate before the worker spawns; log the reason and return false to
    // fail launch() (the dispatcher then falls back to another backend).
    virtual bool precheck(const std::string& binary) = 0;
    virtual std::unique_ptr<fpgaexec::Backend> make_backend() = 0;

private:
    std::thread worker_;

    std::mutex mu_;                              // guards state_/result_
    ExecutionState state_ = ExecutionState::Idle;
    fpgaexec::RunResult result_;
};
