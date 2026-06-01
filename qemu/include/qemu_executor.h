#pragma once

#include "execution_backend.h"
#include "gdb_client.h"
#ifdef USE_PROC_MEM
#include "proc_mem.h"
#endif

#include <memory>
#include <sys/types.h>

// RISC-V Sv39 caps user virtual addresses at 256 GiB (the lower half). qemu-user
// otherwise scatters the guest across the host's 48-bit space (stack ~43 TiB),
// which CVA6's Sv39 MMU cannot represent. We bound the guest address space (-R)
// below this ceiling so checkpoints are loadable on the FPGA, and only capture
// regions within it. See docs/snapshot-interop.md.
inline constexpr uint64_t kSv39UserVaCeiling = 0x4000000000ULL;  // 256 GiB

// The restorer-loader and its blob buffer occupy the top slice of the Sv39
// range, [kRestorerVaBase, kSv39UserVaCeiling). qemu-user places guests far
// below this, so we run the loader's own QEMU with the *same* -R bound — which
// keeps a resumed guest Sv39-legal — and stop capture at this base, excluding
// the loader's own pages from any subsequent checkpoint.
inline constexpr uint64_t kRestorerVaBase = 0x3E00000000ULL;  // 248 GiB

struct QemuConfig {
    std::string qemu_binary = "qemu-riscv64";
    bool enable_gdb = false;
    uint16_t gdb_port = 0; // 0 = auto-select
    std::vector<std::string> qemu_args;
    // -R reserved_va for qemu-user: bounds guest VAs into Sv39 range. 0 disables
    // (used for the restorer-loader, which lives above the ceiling).
    uint64_t reserved_va = kSv39UserVaCeiling;
};

class QemuExecutor : public IExecutionBackend {
public:
    explicit QemuExecutor(QemuConfig config = {});
    ~QemuExecutor() override;

    QemuExecutor(const QemuExecutor&) = delete;
    QemuExecutor& operator=(const QemuExecutor&) = delete;

    bool launch(const std::string& binary,
                const std::vector<std::string>& args) override;

    ExecutionState state() override;
    int wait() override;
    bool stop() override;

    std::optional<ExecutionCheckpoint> checkpoint() override;
    bool restore(const ExecutionCheckpoint& ckpt,
                 const std::string& binary,
                 const std::vector<std::string>& args) override;

    pid_t pid() const { return pid_; }

private:
    QemuConfig config_;
    pid_t pid_ = -1;
    ExecutionState state_ = ExecutionState::Idle;
    int exit_code_ = -1;
    std::unique_ptr<GdbClient> gdb_;
#ifdef USE_PROC_MEM
    std::unique_ptr<ProcMem> proc_mem_;
#endif

    bool trap_to_gdb();
    bool launch_process(const std::string& binary,
                        const std::vector<std::string>& args,
                        uint16_t gdb_port,
                        uint64_t reserved_va);
    uint16_t allocate_port() const;
    void poll_state();

    std::optional<std::vector<uint8_t>> read_guest_memory(uint64_t addr,
                                                          size_t length);
    bool write_guest_memory(uint64_t addr, const std::vector<uint8_t>& data);
};
