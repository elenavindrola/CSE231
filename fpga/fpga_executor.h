// fpga_executor.h — IExecutionBackend over libfpgaexec's FpgaBackend.
//
// Adapts the synchronous one-shot fpgaexec::FpgaBackend::run() (DMA the ELF
// over XDMA, release CVA6 from reset, poll the finisher/UART) to the
// dispatcher's async launch/wait interface by running it on a worker thread.
//
// launch() fails fast — before touching the card — when the binary is not a
// baremetal ELF the bootrom can run (entry != 0x80000000) or the XDMA char
// devices are absent, so the dispatcher can fall back to QEMU instead of
// timing out against missing hardware.

#pragma once

#include "execution_backend.h"
#include "fpgaexec/fpgaexec.hpp"

#include <mutex>
#include <thread>

class FpgaExecutor : public IExecutionBackend {
public:
    explicit FpgaExecutor(fpgaexec::FpgaConfig config = {});
    ~FpgaExecutor() override;

    FpgaExecutor(const FpgaExecutor&) = delete;
    FpgaExecutor& operator=(const FpgaExecutor&) = delete;

    bool launch(const std::string& binary,
                const std::vector<std::string>& args) override;

    ExecutionState state() override;

    // Joins the worker, prints the guest's UART capture to stdout and returns
    // an exit code: 0 PASS, finisher FAIL code (or 1) on FAIL, 2 host error.
    int wait() override;

    // The run cannot be preempted yet: FpgaBackend::run() owns the XDMA
    // handles until the finisher fires or the timeout lapses. Holding CVA6 in
    // reset from a second Xdma instance would race it. M3 (migration) work.
    bool stop() override;

    // FPGA-side snapshot/restore is future M3 work (read registers/memory
    // back over XDMA — see fpgaexec::Xdma).
    std::optional<ExecutionCheckpoint> checkpoint() override;
    bool restore(const ExecutionCheckpoint& ckpt,
                 const std::string& binary,
                 const std::vector<std::string>& args) override;

    // Result of the completed run (valid after wait()).
    const fpgaexec::RunResult& result() const { return result_; }

private:
    fpgaexec::FpgaConfig config_;
    std::thread worker_;

    std::mutex mu_;                              // guards state_/result_
    ExecutionState state_ = ExecutionState::Idle;
    fpgaexec::RunResult result_;

    void set_state(ExecutionState s);
};
