#include "qemu_executor.h"

#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

QemuExecutor::QemuExecutor(QemuConfig config)
    : config_(std::move(config)) {}

QemuExecutor::~QemuExecutor() {
    if (pid_ > 0 && (state_ == ExecutionState::Running ||
                     state_ == ExecutionState::Stopped)) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }
}

uint16_t QemuExecutor::allocate_port() const {
    if (config_.gdb_port > 0) return config_.gdb_port;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 12345;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

bool QemuExecutor::launch_process(const std::string& binary,
                                  const std::vector<std::string>& args,
                                  uint16_t gdb_port,
                                  uint64_t reserved_va) {
    pid_ = fork();
    if (pid_ < 0) {
        state_ = ExecutionState::Error;
        return false;
    }

    if (pid_ == 0) {
        std::vector<std::string> argv_str;
        argv_str.push_back(config_.qemu_binary);

        // Bound the guest address space into Sv39 range so checkpoints are
        // FPGA-loadable (see kSv39UserVaCeiling).
        if (reserved_va > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx",
                          static_cast<unsigned long long>(reserved_va));
            argv_str.push_back("-R");
            argv_str.push_back(buf);
        }

        if (gdb_port > 0) {
            argv_str.push_back("-g");
            argv_str.push_back(std::to_string(gdb_port));
        }

        for (const auto& a : config_.qemu_args)
            argv_str.push_back(a);

        argv_str.push_back(binary);
        for (const auto& a : args)
            argv_str.push_back(a);

        std::vector<char*> argv;
        for (auto& s : argv_str) argv.push_back(s.data());
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    state_ = ExecutionState::Running;
    return true;
}

bool QemuExecutor::launch(const std::string& binary,
                          const std::vector<std::string>& args) {
    if (state_ == ExecutionState::Running) return false;

    uint16_t gdb_port = config_.enable_gdb ? allocate_port() : 0;

    if (!launch_process(binary, args, gdb_port, config_.reserved_va))
        return false;

    if (gdb_port > 0) {
        gdb_ = std::make_unique<GdbClient>("127.0.0.1", gdb_port);
        if (!gdb_->connect()) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
            state_ = ExecutionState::Error;
            return false;
        }
        gdb_->query_halt_reason();
        gdb_->continue_execution();
        // Give QEMU time to enter the execution loop so a subsequent
        // SIGTRAP (from checkpoint/stop) is not lost.
        usleep(20000);
    }

#ifdef USE_PROC_MEM
    proc_mem_ = std::make_unique<ProcMem>(pid_);
    if (!proc_mem_->open()) {
        proc_mem_.reset();
    }
#endif

    return true;
}

void QemuExecutor::poll_state() {
    if (pid_ <= 0 || state_ == ExecutionState::Exited) return;

    int status;
    pid_t ret = waitpid(pid_, &status, WNOHANG);
    if (ret == pid_) {
        state_ = ExecutionState::Exited;
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}

ExecutionState QemuExecutor::state() {
    poll_state();
    return state_;
}

int QemuExecutor::wait() {
    if (pid_ <= 0) return -1;

    int status;
    waitpid(pid_, &status, 0);
    state_ = ExecutionState::Exited;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return exit_code_;
}

bool QemuExecutor::trap_to_gdb() {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (kill(pid_, SIGTRAP) != 0) return false;
        if (gdb_->wait_stop(2000)) return true;
    }
    return false;
}

bool QemuExecutor::stop() {
    if (pid_ <= 0 || state_ != ExecutionState::Running) return false;

    if (gdb_) {
        if (!trap_to_gdb()) return false;
    } else {
        if (kill(pid_, SIGSTOP) != 0) return false;
    }

    state_ = ExecutionState::Stopped;
    return true;
}

#ifdef USE_PROC_MEM
// Distinguish guest memory from QEMU's own host allocations in the shared
// /proc/pid/maps view. In user-mode QEMU the guest's stack and heap appear as
// *anonymous, unlabeled* regions (the guest stack near QEMU's default RV64
// placement, the heap right after the guest data segment) — so they ARE
// captured here. The [stack]/[heap] labels instead belong to QEMU's own host
// stack/heap and must be excluded. The high-address cutoff drops QEMU's host
// libraries/allocations (loaded well above the guest's address range) while
// keeping the guest stack, which sits below it.
//
// The cutoff is kRestorerVaBase (248 GiB), just below the Sv39 ceiling: with -R
// bounding the guest, all real guest memory sits far below it, while QEMU's host
// allocations and the restorer-loader (which lives in the reserved top slice)
// fall outside — so they're never captured into an FPGA-bound blob.
static bool is_guest_region(const MemoryRegion& r) {
    if (r.start >= kRestorerVaBase) return false;
    if (r.pathname.find("[vdso]") != std::string::npos) return false;
    if (r.pathname.find("[vvar]") != std::string::npos) return false;
    if (r.pathname.find("[vsyscall]") != std::string::npos) return false;
    if (r.pathname.find("[stack]") != std::string::npos) return false;  // QEMU host stack
    if (r.pathname.find("[heap]") != std::string::npos) return false;   // QEMU host heap
    if (r.readable && r.writable && r.executable) return false;
    return true;
}

static uint32_t region_prot(const MemoryRegion& r) {
    uint32_t prot = 0;
    if (r.readable)   prot |= MemoryRegionSnapshot::PROT_R;
    if (r.writable)   prot |= MemoryRegionSnapshot::PROT_W;
    if (r.executable) prot |= MemoryRegionSnapshot::PROT_X;
    return prot;
}

static bool region_is_anonymous(const MemoryRegion& r) {
    // Pseudo-files like "[anon:...]" and empty pathnames are anonymous.
    return r.pathname.empty() || r.pathname.front() == '[';
}
#endif

std::optional<ExecutionCheckpoint> QemuExecutor::checkpoint() {
    if (pid_ <= 0) return std::nullopt;

    if (!gdb_) return std::nullopt;

    if (state_ == ExecutionState::Running) {
        if (!trap_to_gdb()) return std::nullopt;
        state_ = ExecutionState::Stopped;
    }

    auto regs = gdb_->read_registers();
    if (!regs) return std::nullopt;

    ExecutionCheckpoint ckpt;
    ckpt.registers = *regs;

    // guest_base maps host addresses (from /proc/pid/maps) to guest virtual
    // addresses: guest = host - guest_base. For statically-linked RV64 ELFs
    // under qemu-riscv64 this is 0 (the guest image is mapped 1:1), so the
    // host addresses below already are guest VAs. The field is recorded
    // explicitly so the format stays correct if a future loader relocates the
    // guest; nonzero-base detection is left as follow-on work.
    ckpt.guest_base = 0;

#ifdef USE_PROC_MEM
    if (proc_mem_) {
        auto regions = proc_mem_->regions();
        if (regions) {
            for (const auto& r : *regions) {
                if (!r.readable || !is_guest_region(r))
                    continue;
                auto data = proc_mem_->read(r.start, r.size());
                if (!data) continue;

                MemoryRegionSnapshot snap;
                snap.addr        = r.start - ckpt.guest_base;
                snap.prot        = region_prot(r);
                snap.anonymous   = region_is_anonymous(r);
                snap.file_offset = snap.anonymous ? 0 : r.offset;
                snap.path        = snap.anonymous ? std::string{} : r.pathname;
                snap.data        = std::move(*data);
                ckpt.memory.push_back(std::move(snap));
            }
        }
    }
#endif

    return ckpt;
}

// Path to the restorer-loader ELF: runtime override first, then the path
// baked in by CMake, then bare PATH lookup.
static std::string restorer_path() {
    if (const char* e = std::getenv("RISCV_RESTORER_BIN"); e && *e) return e;
#ifdef RESTORER_RV_PATH
    return RESTORER_RV_PATH;
#else
    return "restorer_rv";
#endif
}

bool QemuExecutor::restore(const ExecutionCheckpoint& ckpt,
                           const std::string& /*binary*/,
                           const std::vector<std::string>& /*args*/) {
    // Reconstruction is replay-free: the loader rebuilds the entire address
    // space from the checkpoint, so the original binary/args are unused.
    if (pid_ > 0 && (state_ == ExecutionState::Running ||
                     state_ == ExecutionState::Stopped)) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }
    gdb_.reset();
#ifdef USE_PROC_MEM
    proc_mem_.reset();
#endif

    // Serialize the checkpoint to a temp blob for the loader to read.
    char blob_path[] = "/tmp/rvckpt_XXXXXX";
    int bfd = mkstemp(blob_path);
    if (bfd < 0) { state_ = ExecutionState::Error; return false; }
    ::close(bfd);
    if (!ckpt.serialize(blob_path)) {
        unlink(blob_path);
        state_ = ExecutionState::Error;
        return false;
    }

    auto fail = [&]() {
        if (pid_ > 0) { kill(pid_, SIGKILL); waitpid(pid_, nullptr, 0); }
        pid_ = -1;
        gdb_.reset();
        unlink(blob_path);
        state_ = ExecutionState::Error;
        return false;
    };

    // Launch the restorer-loader under QEMU with the GDB stub. It mmaps every
    // checkpoint region at its guest VA, copies the bytes in, then ebreaks.
    // Same -R as the workload: the loader lives in the reserved top slice
    // ([kRestorerVaBase, ceiling)), clear of the regions it reconstructs, and
    // this keeps the *resumed* guest Sv39-bounded too.
    uint16_t port = allocate_port();
    if (!launch_process(restorer_path(), {blob_path}, port, config_.reserved_va))
        return fail();

    gdb_ = std::make_unique<GdbClient>("127.0.0.1", port);
    if (!gdb_->connect()) return fail();
    gdb_->query_halt_reason();

    // Run the loader; it traps (ebreak) once the address space is rebuilt.
    if (!gdb_->continue_execution()) return fail();
    if (!gdb_->wait_stop(10000)) return fail();

    // Memory is reconstructed. Overlay the architectural registers (incl. PC)
    // and resume the guest at the checkpoint PC.
    if (!gdb_->write_registers(ckpt.registers)) return fail();
    if (!gdb_->continue_execution()) return fail();
    usleep(20000);

#ifdef USE_PROC_MEM
    proc_mem_ = std::make_unique<ProcMem>(pid_);
    if (!proc_mem_->open()) proc_mem_.reset();
#endif

    unlink(blob_path);
    state_ = ExecutionState::Running;
    return true;
}

std::optional<std::vector<uint8_t>> QemuExecutor::read_guest_memory(
    uint64_t addr, size_t length) {
#ifdef USE_PROC_MEM
    if (proc_mem_) return proc_mem_->read(addr, length);
    return std::nullopt;
#else
    if (gdb_) return gdb_->read_memory(addr, length);
    return std::nullopt;
#endif
}

bool QemuExecutor::write_guest_memory(uint64_t addr,
                                      const std::vector<uint8_t>& data) {
#ifdef USE_PROC_MEM
    if (proc_mem_) return proc_mem_->write(addr, data);
    return false;
#else
    if (gdb_) return gdb_->write_memory(addr, data);
    return false;
#endif
}
