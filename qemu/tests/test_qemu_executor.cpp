#include "qemu_executor.h"
#include "gdb_client.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

static std::string find_binary(const char* env_var, const char* fallback_name) {
    const char* env = std::getenv(env_var);
    if (env) return env;

    auto candidate = fs::path(__FILE__).parent_path() / fallback_name;
    if (fs::exists(candidate)) return candidate.string();

    return "";
}

static uint16_t allocate_ephemeral_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
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

class QemuExecutorTest : public ::testing::Test {
protected:
    std::string hello_binary;
    std::string spin_binary;
    std::string pattern_binary;
    std::string memload_binary;

    void SetUp() override {
        if (std::system("which qemu-riscv64 > /dev/null 2>&1") != 0)
            GTEST_SKIP() << "qemu-riscv64 not found in PATH";

        hello_binary = find_binary("RISCV_TEST_BIN", "hello_rv");
        spin_binary = find_binary("RISCV_SPIN_BIN", "spin_rv");
        pattern_binary = find_binary("RISCV_PATTERN_BIN", "mempattern_rv");
        memload_binary = find_binary("RISCV_MEMLOAD_BIN", "memload_rv");

        if (hello_binary.empty() || !fs::exists(hello_binary))
            GTEST_SKIP() << "No hello_rv test binary. Set RISCV_TEST_BIN.";
        if (spin_binary.empty() || !fs::exists(spin_binary))
            GTEST_SKIP() << "No spin_rv test binary. Set RISCV_SPIN_BIN.";
    }
};

TEST_F(QemuExecutorTest, LaunchAndWait) {
    QemuConfig config;
    config.enable_gdb = false;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(hello_binary, {}));
    EXPECT_GT(qemu.pid(), 0);

    int exit_code = qemu.wait();
    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(qemu.state(), ExecutionState::Exited);
}

TEST_F(QemuExecutorTest, LaunchWithArgs) {
    QemuConfig config;
    config.enable_gdb = false;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(hello_binary, {"arg1", "arg2"}));
    int exit_code = qemu.wait();
    EXPECT_EQ(exit_code, 0);
}

TEST_F(QemuExecutorTest, LaunchWithGdb) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));
    EXPECT_EQ(qemu.state(), ExecutionState::Running);

    ASSERT_TRUE(qemu.stop());
    EXPECT_EQ(qemu.state(), ExecutionState::Stopped);
}

TEST_F(QemuExecutorTest, Checkpoint) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());
    EXPECT_NE(ckpt->registers.pc, 0u);
}

TEST_F(QemuExecutorTest, CheckpointSerializeDeserialize) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());

    std::string tmp = std::tmpnam(nullptr);
    ASSERT_TRUE(ckpt->registers.serialize(tmp));

    auto loaded = RiscvContext::deserialize(tmp);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->pc, ckpt->registers.pc);

    std::remove(tmp.c_str());
}

// Full-checkpoint format: registers + CSR block + guest_base + memory regions
// (with metadata) must round-trip through the on-disk blob byte-for-byte.
TEST_F(QemuExecutorTest, FullCheckpointSerializeDeserialize) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());

    std::string tmp = std::tmpnam(nullptr);
    ASSERT_TRUE(ckpt->serialize(tmp));

    auto loaded = ExecutionCheckpoint::deserialize(tmp);
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->registers.pc, ckpt->registers.pc);
    EXPECT_EQ(loaded->registers.gpr, ckpt->registers.gpr);
    EXPECT_EQ(loaded->guest_base, ckpt->guest_base);
    EXPECT_EQ(loaded->csr.present, ckpt->csr.present);
    ASSERT_EQ(loaded->memory.size(), ckpt->memory.size());

    for (size_t i = 0; i < loaded->memory.size(); i++) {
        const auto& a = ckpt->memory[i];
        const auto& b = loaded->memory[i];
        EXPECT_EQ(b.addr, a.addr);
        EXPECT_EQ(b.prot, a.prot);
        EXPECT_EQ(b.anonymous, a.anonymous);
        EXPECT_EQ(b.file_offset, a.file_offset);
        EXPECT_EQ(b.path, a.path);
        EXPECT_EQ(b.data, a.data);
    }

#ifdef USE_PROC_MEM
    // The /proc/pid/mem path must actually capture guest memory (stack, heap,
    // data) — not an empty region table.
    EXPECT_FALSE(loaded->memory.empty());
#endif

    std::remove(tmp.c_str());
}

TEST_F(QemuExecutorTest, RestoreFromCheckpoint) {
    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());

    ASSERT_TRUE(qemu.restore(*ckpt, spin_binary, {}));
    EXPECT_EQ(qemu.state(), ExecutionState::Running);
}

TEST_F(QemuExecutorTest, DoublelaunchFails) {
    QemuConfig config;
    config.enable_gdb = false;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(hello_binary, {}));
    EXPECT_FALSE(qemu.launch(hello_binary, {}));
    qemu.wait();
}

TEST_F(QemuExecutorTest, StopWithoutGdb) {
    QemuConfig config;
    config.enable_gdb = false;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));
    ASSERT_TRUE(qemu.stop());
    EXPECT_EQ(qemu.state(), ExecutionState::Stopped);
}

TEST_F(QemuExecutorTest, CheckpointWithoutGdbFails) {
    QemuConfig config;
    config.enable_gdb = false;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));
    usleep(50000);

    auto ctx = qemu.checkpoint();
    EXPECT_FALSE(ctx.has_value());
}

TEST_F(QemuExecutorTest, RegisterWriteReadRoundTrip) {
    // Write registers through QEMU's GDB stub and read them back with no
    // guest execution in between. This is the core data path a context
    // switch relies on — if the encoding is wrong, registers silently
    // corrupt on migration.

    uint16_t port = allocate_ephemeral_port();
    ASSERT_GT(port, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        std::string port_str = std::to_string(port);
        execlp("qemu-riscv64", "qemu-riscv64",
               "-g", port_str.c_str(),
               spin_binary.c_str(), nullptr);
        _exit(127);
    }

    GdbClient gdb("127.0.0.1", port);
    ASSERT_TRUE(gdb.connect(5000));
    gdb.query_halt_reason();

    auto initial = gdb.read_registers();
    ASSERT_TRUE(initial.has_value());

    RiscvContext written{};
    written.pc = initial->pc;
    written.gpr[0] = 0;
    for (int i = 1; i < 32; i++)
        written.gpr[i] = 0xA000000000000000ULL | (static_cast<uint64_t>(i) << 32);
    for (int i = 0; i < 32; i++)
        written.fpr[i] = 0xF000000000000000ULL | (static_cast<uint64_t>(i) << 32);
    written.fcsr = 0x1F;

    ASSERT_TRUE(gdb.write_registers(written));

    auto readback = gdb.read_registers();
    ASSERT_TRUE(readback.has_value());

    EXPECT_EQ(readback->pc, written.pc) << "pc";
    EXPECT_EQ(readback->gpr[0], 0u) << "x0 must be zero";
    for (int i = 1; i < 32; i++)
        EXPECT_EQ(readback->gpr[i], written.gpr[i]) << "gpr[" << i << "]";
    for (int i = 0; i < 32; i++)
        EXPECT_EQ(readback->fpr[i], written.fpr[i]) << "fpr[" << i << "]";
    EXPECT_EQ(readback->fcsr, written.fcsr) << "fcsr";

    gdb.disconnect();
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

TEST_F(QemuExecutorTest, ContextSwitchPreservesRegisters) {
    // Checkpoint → restore → checkpoint through QemuExecutor.
    // With memory restore, all callee-saved GPRs (sp, s0-s11) should
    // survive the round-trip since their stack frames are restored.

    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(spin_binary, {}));

    auto before = qemu.checkpoint();
    ASSERT_TRUE(before.has_value());

    bool has_memory = !before->memory.empty();

    ASSERT_TRUE(qemu.restore(*before, spin_binary, {}));

    auto after = qemu.checkpoint();
    ASSERT_TRUE(after.has_value());

    EXPECT_EQ(after->registers.gpr[0], 0u) << "x0 hardwired zero";

    EXPECT_EQ(after->registers.gpr[3], before->registers.gpr[3]) << "gp (x3)";
    EXPECT_EQ(after->registers.gpr[4], before->registers.gpr[4]) << "tp (x4)";

    const std::pair<int,const char*> callee_saved[] = {
        {2,"sp"}, {8,"s0"}, {9,"s1"},
        {18,"s2"}, {19,"s3"}, {20,"s4"}, {21,"s5"}, {22,"s6"},
        {23,"s7"}, {24,"s8"}, {25,"s9"}, {26,"s10"}, {27,"s11"},
    };

    if (has_memory) {
        for (auto [reg, name] : callee_saved)
            EXPECT_EQ(after->registers.gpr[reg], before->registers.gpr[reg]) << name;
    } else {
        int mismatches = 0;
        for (auto [reg, name] : callee_saved)
            if (after->registers.gpr[reg] != before->registers.gpr[reg])
                mismatches++;
        if (mismatches > 0)
            printf("  NOTE: %d callee-saved GPRs differ without memory restore\n",
                   mismatches);
    }

    for (int i = 0; i < 32; i++)
        EXPECT_EQ(after->registers.fpr[i], before->registers.fpr[i]) << "fpr[" << i << "]";
    EXPECT_EQ(after->registers.fcsr, before->registers.fcsr) << "fcsr";
}

TEST_F(QemuExecutorTest, PostRestoreMemoryCorrectness) {
    // Checkpoint registers + memory via the executor, tamper with the
    // checkpoint's scratch values, restore to a fresh QEMU, and verify
    // the modified data survived.  Proves memory restore is real, not
    // just ELF re-initialization.

    if (pattern_binary.empty() || !fs::exists(pattern_binary))
        GTEST_SKIP() << "No mempattern_rv binary. Set RISCV_PATTERN_BIN.";

    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(pattern_binary, {}));

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());
    if (ckpt->memory.empty())
        GTEST_SKIP() << "No guest memory captured (USE_PROC_MEM off?)";


    // Find the marker struct in the checkpoint's memory
    const uint64_t marker = 0xDEADBEEFCAFEBABE;
    const uint64_t orig_scratch[] = {0x1111, 0x2222, 0x3333, 0x4444};
    int marker_idx = -1;
    size_t marker_off = 0;

    for (int s = 0; s < static_cast<int>(ckpt->memory.size()); s++) {
        auto& region = ckpt->memory[s];
        for (size_t i = 0; i + 40 <= region.data.size(); i += 8) {
            uint64_t val;
            memcpy(&val, region.data.data() + i, sizeof(val));
            if (val != marker) continue;
            uint64_t sc[4];
            memcpy(sc, region.data.data() + i + 8, 32);
            if (sc[0] == orig_scratch[0] && sc[1] == orig_scratch[1] &&
                sc[2] == orig_scratch[2] && sc[3] == orig_scratch[3]) {
                marker_idx = s;
                marker_off = i;
                break;
            }
        }
        if (marker_idx >= 0) break;
    }
    ASSERT_GE(marker_idx, 0) << "Marker struct not found in checkpoint memory";

    // Modify scratch in the checkpoint to prove restore writes real data
    const uint64_t mod_scratch[] = {0xAAAA1111, 0xBBBB2222, 0xCCCC3333, 0xDDDD4444};
    for (int i = 0; i < 4; i++)
        memcpy(ckpt->memory[marker_idx].data.data() + marker_off + 8 + i * 8,
               &mod_scratch[i], sizeof(uint64_t));

    // Restore to a fresh QEMU instance
    ASSERT_TRUE(qemu.restore(*ckpt, pattern_binary, {}));
    EXPECT_EQ(qemu.state(), ExecutionState::Running);

    // Stop and verify the modified scratch values survived
    auto after = qemu.checkpoint();
    ASSERT_TRUE(after.has_value());

    // Verify modified scratch values in the restored memory
    bool found = false;
    for (const auto& region : after->memory) {
        for (size_t i = 0; i + 40 <= region.data.size(); i += 8) {
            uint64_t val;
            memcpy(&val, region.data.data() + i, sizeof(val));
            if (val != marker) continue;
            uint64_t sc[4];
            memcpy(sc, region.data.data() + i + 8, 32);
            if (sc[0] == mod_scratch[0] && sc[1] == mod_scratch[1] &&
                sc[2] == mod_scratch[2] && sc[3] == mod_scratch[3]) {
                found = true;
                break;
            }
        }
        if (found) break;
    }
    EXPECT_TRUE(found) << "Modified marker struct not found after restore";
}

// Reconstruct a *runtime-allocated* region. memload_rv malloc()s a multi-MB
// buffer (glibc routes large allocations to mmap) and fills it with 0xAB. That
// mapping exists only because of runtime allocation — so this exercises the
// loader recreating a mapping from the checkpoint, not the old path's reliance
// on replaying the binary to recreate its own layout.
TEST_F(QemuExecutorTest, RestoreReconstructsDynamicAllocation) {
#ifndef USE_PROC_MEM
    GTEST_SKIP() << "memory capture requires USE_PROC_MEM";
#else
    if (memload_binary.empty() || !fs::exists(memload_binary))
        GTEST_SKIP() << "No memload_rv binary. Set RISCV_MEMLOAD_BIN.";

    QemuConfig config;
    config.enable_gdb = true;
    QemuExecutor qemu(config);

    ASSERT_TRUE(qemu.launch(memload_binary, {"8"}));  // 8 MiB buffer
    usleep(300000);                                   // let it malloc + memset

    auto ckpt = qemu.checkpoint();
    ASSERT_TRUE(ckpt.has_value());
    if (ckpt->memory.empty())
        GTEST_SKIP() << "No guest memory captured (USE_PROC_MEM off?)";

    // Locate the large 0xAB-filled runtime allocation (sample mid + end to skip
    // any malloc header at the front).
    int idx = -1;
    for (size_t s = 0; s < ckpt->memory.size(); s++) {
        const auto& d = ckpt->memory[s].data;
        if (d.size() < 4u * 1024 * 1024) continue;
        // Sample interior points (the tail page holds malloc metadata, not 0xAB).
        if (d[d.size() / 4] == 0xAB && d[d.size() / 2] == 0xAB) { idx = (int)s; break; }
    }
    ASSERT_GE(idx, 0) << "runtime 0xAB allocation not found in checkpoint";

    const uint64_t region_addr = ckpt->memory[idx].addr;
    const size_t off = ckpt->memory[idx].data.size() / 2;
    const uint8_t sentinel[16] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED,0xFA,0xCE,
                                  0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    memcpy(ckpt->memory[idx].data.data() + off, sentinel, sizeof(sentinel));

    // Restore via the loader (no replay) and confirm the sentinel survived in
    // the reconstructed dynamic region.
    ASSERT_TRUE(qemu.restore(*ckpt, memload_binary, {"8"}));
    EXPECT_EQ(qemu.state(), ExecutionState::Running);

    auto after = qemu.checkpoint();
    ASSERT_TRUE(after.has_value());

    bool found = false;
    for (const auto& region : after->memory) {
        if (region.addr != region_addr) continue;
        if (region.data.size() >= off + sizeof(sentinel) &&
            memcmp(region.data.data() + off, sentinel, sizeof(sentinel)) == 0)
            found = true;
        break;
    }
    EXPECT_TRUE(found) << "sentinel not found in reconstructed dynamic region";
#endif
}
