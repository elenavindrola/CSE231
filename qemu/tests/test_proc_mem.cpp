#include "proc_mem.h"

#include <csignal>
#include <cstring>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

// Spawns a native child process that sleeps, giving us a stable target
// for /proc/pid/mem reads.
class ProcMemTest : public ::testing::Test {
protected:
    pid_t child_pid = -1;

    void SetUp() override {
        child_pid = fork();
        if (child_pid == 0) {
            pause();
            _exit(0);
        }
        ASSERT_GT(child_pid, 0);
        usleep(50000);
    }

    void TearDown() override {
        if (child_pid > 0) {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, nullptr, 0);
        }
    }
};

TEST_F(ProcMemTest, OpenClose) {
    ProcMem pm(child_pid);
    EXPECT_FALSE(pm.is_open());
    ASSERT_TRUE(pm.open());
    EXPECT_TRUE(pm.is_open());
    pm.close();
    EXPECT_FALSE(pm.is_open());
}

TEST_F(ProcMemTest, DoubleOpen) {
    ProcMem pm(child_pid);
    ASSERT_TRUE(pm.open());
    ASSERT_TRUE(pm.open());
    EXPECT_TRUE(pm.is_open());
}

TEST_F(ProcMemTest, OpenInvalidPid) {
    ProcMem pm(999999999);
    EXPECT_FALSE(pm.open());
}

TEST_F(ProcMemTest, ReadWithoutOpen) {
    ProcMem pm(child_pid);
    auto result = pm.read(0x1000, 4);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ProcMemTest, WriteWithoutOpen) {
    ProcMem pm(child_pid);
    std::vector<uint8_t> data = {0x01};
    EXPECT_FALSE(pm.write(0x1000, data));
}

TEST_F(ProcMemTest, ReadInvalidAddress) {
    ProcMem pm(child_pid);
    ASSERT_TRUE(pm.open());
    auto result = pm.read(0xDEAD, 4);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ProcMemTest, Regions) {
    ProcMem pm(child_pid);
    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());
    EXPECT_GT(regs->size(), 0u);

    bool found_executable = false;
    bool found_stack = false;
    for (const auto& r : *regs) {
        EXPECT_LT(r.start, r.end);
        if (r.executable) found_executable = true;
        if (r.pathname.find("[stack]") != std::string::npos) found_stack = true;
    }
    EXPECT_TRUE(found_executable);
    EXPECT_TRUE(found_stack);
}

TEST_F(ProcMemTest, ReadValidRegion) {
    ProcMem pm(child_pid);
    ASSERT_TRUE(pm.open());

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());

    // Find a readable region
    for (const auto& r : *regs) {
        if (!r.readable) continue;
        size_t len = std::min<size_t>(r.size(), 64);
        auto data = pm.read(r.start, len);
        ASSERT_TRUE(data.has_value()) << "Failed to read from " << std::hex << r.start;
        EXPECT_EQ(data->size(), len);
        return;
    }
    FAIL() << "No readable region found";
}

TEST_F(ProcMemTest, WriteReadRoundTrip) {
    ProcMem pm(child_pid);
    ASSERT_TRUE(pm.open());

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());

    // Find a writable region (not the stack — use heap or anon mmap)
    for (const auto& r : *regs) {
        if (!r.readable || !r.writable) continue;
        if (r.pathname.find("[stack]") != std::string::npos) continue;

        std::vector<uint8_t> pattern = {0xAA, 0xBB, 0xCC, 0xDD};

        auto original = pm.read(r.start, pattern.size());
        ASSERT_TRUE(original.has_value());

        ASSERT_TRUE(pm.write(r.start, pattern));

        auto readback = pm.read(r.start, pattern.size());
        ASSERT_TRUE(readback.has_value());
        EXPECT_EQ(*readback, pattern);

        // Restore original
        pm.write(r.start, *original);
        return;
    }
    GTEST_SKIP() << "No writable non-stack region found";
}

// --- QEMU + proc_mem integration tests ---

namespace fs = std::filesystem;

static std::string find_binary(const char* env_var, const char* fallback) {
    const char* env = std::getenv(env_var);
    if (env) return env;
    auto p = fs::path(__FILE__).parent_path() / fallback;
    return fs::exists(p) ? p.string() : "";
}

class QemuProcMemTest : public ::testing::Test {
protected:
    std::string spin_binary;
    std::string pattern_binary;
    pid_t qemu_pid = -1;

    void SetUp() override {
        if (std::system("which qemu-riscv64 > /dev/null 2>&1") != 0)
            GTEST_SKIP() << "qemu-riscv64 not found";

        spin_binary = find_binary("RISCV_SPIN_BIN", "spin_rv");
        pattern_binary = find_binary("RISCV_PATTERN_BIN", "mempattern_rv");

        if (spin_binary.empty() || !fs::exists(spin_binary))
            GTEST_SKIP() << "No spin_rv binary";
        if (pattern_binary.empty() || !fs::exists(pattern_binary))
            GTEST_SKIP() << "No mempattern_rv binary";
    }

    void TearDown() override {
        if (qemu_pid > 0) {
            kill(qemu_pid, SIGKILL);
            waitpid(qemu_pid, nullptr, 0);
        }
    }

    pid_t launch_qemu(const std::string& binary) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("qemu-riscv64", "qemu-riscv64", binary.c_str(), nullptr);
            _exit(127);
        }
        usleep(100000);
        qemu_pid = pid;
        return pid;
    }
};

TEST_F(QemuProcMemTest, OpenQemuProcess) {
    pid_t pid = launch_qemu(spin_binary);
    ProcMem pm(pid);
    ASSERT_TRUE(pm.open());
    EXPECT_TRUE(pm.is_open());
}

TEST_F(QemuProcMemTest, ReadQemuRegions) {
    pid_t pid = launch_qemu(spin_binary);
    ProcMem pm(pid);

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());
    EXPECT_GT(regs->size(), 0u);

    bool found_qemu = false;
    for (const auto& r : *regs) {
        if (r.pathname.find("qemu") != std::string::npos) {
            found_qemu = true;
            break;
        }
    }
    EXPECT_TRUE(found_qemu) << "Expected to see qemu in memory map";
}

TEST_F(QemuProcMemTest, ReadQemuMemory) {
    pid_t pid = launch_qemu(spin_binary);
    ProcMem pm(pid);
    ASSERT_TRUE(pm.open());

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());

    for (const auto& r : *regs) {
        if (!r.readable) continue;
        auto data = pm.read(r.start, 16);
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(data->size(), 16u);
        return;
    }
    FAIL() << "No readable region";
}

TEST_F(QemuProcMemTest, FindMarkerInGuestMemory) {
    pid_t pid = launch_qemu(pattern_binary);
    ProcMem pm(pid);
    ASSERT_TRUE(pm.open());

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());

    const uint64_t marker = 0xDEADBEEFCAFEBABE;
    const uint64_t expected_scratch[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    // The struct is 40 bytes: marker(8) + scratch(32)
    constexpr size_t STRUCT_SIZE = 8 + 32;
    bool found = false;

    for (const auto& r : *regs) {
        if (!r.readable || r.size() > 64 * 1024 * 1024) continue;

        auto data = pm.read(r.start, r.size());
        if (!data) continue;

        for (size_t i = 0; i + STRUCT_SIZE <= data->size(); i += 8) {
            uint64_t val;
            memcpy(&val, data->data() + i, sizeof(val));
            if (val != marker) continue;

            // Validate by checking scratch values match — the marker may
            // appear in QEMU's code cache or other internal structures.
            uint64_t s[4];
            memcpy(s, data->data() + i + 8, 32);
            if (s[0] == expected_scratch[0] && s[1] == expected_scratch[1] &&
                s[2] == expected_scratch[2] && s[3] == expected_scratch[3]) {
                found = true;
                break;
            }
        }
        if (found) break;
    }
    EXPECT_TRUE(found) << "Could not find marker+scratch struct in QEMU process memory";
}

TEST_F(QemuProcMemTest, WriteGuestMemory) {
    pid_t pid = launch_qemu(pattern_binary);
    ProcMem pm(pid);
    ASSERT_TRUE(pm.open());

    auto regs = pm.regions();
    ASSERT_TRUE(regs.has_value());

    const uint64_t marker = 0xDEADBEEFCAFEBABE;

    for (const auto& r : *regs) {
        if (!r.readable || !r.writable || r.size() > 64 * 1024 * 1024) continue;

        auto data = pm.read(r.start, r.size());
        if (!data) continue;

        for (size_t i = 0; i + sizeof(marker) <= data->size(); i += 8) {
            uint64_t val;
            memcpy(&val, data->data() + i, sizeof(val));
            if (val != marker) continue;

            // Overwrite the scratch values
            uint64_t new_vals[4] = {0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD};
            std::vector<uint8_t> new_data(32);
            memcpy(new_data.data(), new_vals, 32);
            uint64_t scratch_addr = r.start + i + 8;

            ASSERT_TRUE(pm.write(scratch_addr, new_data));

            auto readback = pm.read(scratch_addr, 32);
            ASSERT_TRUE(readback.has_value());

            uint64_t rb[4];
            memcpy(rb, readback->data(), 32);
            EXPECT_EQ(rb[0], 0xAAAAu);
            EXPECT_EQ(rb[1], 0xBBBBu);
            EXPECT_EQ(rb[2], 0xCCCCu);
            EXPECT_EQ(rb[3], 0xDDDDu);
            return;
        }
    }
    GTEST_SKIP() << "No writable region with marker found";
}
