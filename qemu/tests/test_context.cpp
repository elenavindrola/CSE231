#include "execution_context.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

class ContextTest : public ::testing::Test {
protected:
    std::string tmp_path;

    void SetUp() override {
        tmp_path = std::filesystem::temp_directory_path() / "ctx_test_XXXXXX";
        tmp_path = std::tmpnam(nullptr);
    }

    void TearDown() override { std::remove(tmp_path.c_str()); }
};

TEST_F(ContextTest, SerializeDeserializeRoundTrip) {
    RiscvContext ctx{};
    ctx.pc = 0xDEADBEEF'CAFEBABE;
    for (int i = 0; i < 32; i++) ctx.gpr[i] = 0x1000 + i;
    for (int i = 0; i < 32; i++) ctx.fpr[i] = 0x2000 + i;
    ctx.fcsr = 0x42;

    ASSERT_TRUE(ctx.serialize(tmp_path));

    auto loaded = RiscvContext::deserialize(tmp_path);
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->pc, ctx.pc);
    EXPECT_EQ(loaded->fcsr, ctx.fcsr);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(loaded->gpr[i], ctx.gpr[i]) << "gpr[" << i << "] mismatch";
        EXPECT_EQ(loaded->fpr[i], ctx.fpr[i]) << "fpr[" << i << "] mismatch";
    }
}

TEST_F(ContextTest, DeserializeBadMagic) {
    {
        std::ofstream out(tmp_path, std::ios::binary);
        out << "BADMAGIC";
        for (int i = 0; i < 600; i++) out.put('\0');
    }
    auto result = RiscvContext::deserialize(tmp_path);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ContextTest, DeserializeNonexistentFile) {
    auto result = RiscvContext::deserialize("/nonexistent/path/ctx.bin");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ContextTest, DeserializeTruncated) {
    {
        std::ofstream out(tmp_path, std::ios::binary);
        out.write("RVCTX100", 8);
        uint64_t pc = 0x1234;
        out.write(reinterpret_cast<const char*>(&pc), sizeof(pc));
        // Missing GPR/FPR/FCSR data
    }
    auto result = RiscvContext::deserialize(tmp_path);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ContextTest, ZeroContext) {
    RiscvContext ctx{};
    ASSERT_TRUE(ctx.serialize(tmp_path));

    auto loaded = RiscvContext::deserialize(tmp_path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->pc, 0u);
    EXPECT_EQ(loaded->fcsr, 0u);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(loaded->gpr[i], 0u);
        EXPECT_EQ(loaded->fpr[i], 0u);
    }
}

TEST_F(ContextTest, MaxValues) {
    RiscvContext ctx{};
    ctx.pc = UINT64_MAX;
    for (int i = 0; i < 32; i++) ctx.gpr[i] = UINT64_MAX;
    for (int i = 0; i < 32; i++) ctx.fpr[i] = UINT64_MAX;
    ctx.fcsr = UINT32_MAX;

    ASSERT_TRUE(ctx.serialize(tmp_path));

    auto loaded = RiscvContext::deserialize(tmp_path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->pc, UINT64_MAX);
    EXPECT_EQ(loaded->fcsr, UINT32_MAX);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(loaded->gpr[i], UINT64_MAX);
        EXPECT_EQ(loaded->fpr[i], UINT64_MAX);
    }
}
