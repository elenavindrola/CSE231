#include "execution_context.h"

#include <cstring>
#include <fstream>
#include <type_traits>

static constexpr char MAGIC[8] = {'R','V','C','T','X','1','0','0'};

bool RiscvContext::serialize(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(MAGIC, sizeof(MAGIC));
    out.write(reinterpret_cast<const char*>(&pc), sizeof(pc));
    out.write(reinterpret_cast<const char*>(gpr.data()),
              gpr.size() * sizeof(uint64_t));
    out.write(reinterpret_cast<const char*>(fpr.data()),
              fpr.size() * sizeof(uint64_t));
    out.write(reinterpret_cast<const char*>(&fcsr), sizeof(fcsr));

    return out.good();
}

std::optional<RiscvContext> RiscvContext::deserialize(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    char magic[8];
    in.read(magic, sizeof(magic));
    if (std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0)
        return std::nullopt;

    RiscvContext ctx;
    in.read(reinterpret_cast<char*>(&ctx.pc), sizeof(ctx.pc));
    in.read(reinterpret_cast<char*>(ctx.gpr.data()),
            ctx.gpr.size() * sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(ctx.fpr.data()),
            ctx.fpr.size() * sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(&ctx.fcsr), sizeof(ctx.fcsr));

    if (!in.good()) return std::nullopt;
    return ctx;
}

// ---- ExecutionCheckpoint: full process image (registers + memory) ----

static constexpr char CKPT_MAGIC[8] = {'R','V','C','K','P','T','0','1'};

namespace {

template <typename T>
void put(std::ofstream& out, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
bool get(std::ifstream& in, T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(in);
}

void put_str(std::ofstream& out, const std::string& s) {
    put<uint32_t>(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool get_str(std::ifstream& in, std::string& s) {
    uint32_t len = 0;
    if (!get(in, len)) return false;
    s.resize(len);
    if (len) in.read(s.data(), len);
    return static_cast<bool>(in);
}

}  // namespace

bool ExecutionCheckpoint::serialize(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(CKPT_MAGIC, sizeof(CKPT_MAGIC));

    // Architectural registers (same field order as RiscvContext's own format).
    put(out, registers.pc);
    out.write(reinterpret_cast<const char*>(registers.gpr.data()),
              registers.gpr.size() * sizeof(uint64_t));
    out.write(reinterpret_cast<const char*>(registers.fpr.data()),
              registers.fpr.size() * sizeof(uint64_t));
    put(out, registers.fcsr);

    // Machine CSR block (zero-filled on the user-mode side).
    put<uint8_t>(out, csr.present ? 1 : 0);
    put(out, csr.mstatus);
    put(out, csr.mepc);
    put(out, csr.satp);
    out.write(reinterpret_cast<const char*>(csr.reserved.data()),
              csr.reserved.size() * sizeof(uint64_t));

    put(out, guest_base);

    // Region table.
    put<uint64_t>(out, memory.size());
    for (const auto& r : memory) {
        put(out, r.addr);
        put(out, r.prot);
        put<uint8_t>(out, r.anonymous ? 1 : 0);
        put(out, r.file_offset);
        put_str(out, r.path);
        put<uint64_t>(out, r.data.size());
        out.write(reinterpret_cast<const char*>(r.data.data()),
                  static_cast<std::streamsize>(r.data.size()));
    }

    return out.good();
}

std::optional<ExecutionCheckpoint> ExecutionCheckpoint::deserialize(
    const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    char magic[8];
    in.read(magic, sizeof(magic));
    if (std::memcmp(magic, CKPT_MAGIC, sizeof(CKPT_MAGIC)) != 0)
        return std::nullopt;

    ExecutionCheckpoint ckpt;

    if (!get(in, ckpt.registers.pc)) return std::nullopt;
    in.read(reinterpret_cast<char*>(ckpt.registers.gpr.data()),
            ckpt.registers.gpr.size() * sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(ckpt.registers.fpr.data()),
            ckpt.registers.fpr.size() * sizeof(uint64_t));
    if (!get(in, ckpt.registers.fcsr)) return std::nullopt;

    uint8_t present = 0;
    if (!get(in, present)) return std::nullopt;
    ckpt.csr.present = present != 0;
    if (!get(in, ckpt.csr.mstatus)) return std::nullopt;
    if (!get(in, ckpt.csr.mepc)) return std::nullopt;
    if (!get(in, ckpt.csr.satp)) return std::nullopt;
    in.read(reinterpret_cast<char*>(ckpt.csr.reserved.data()),
            ckpt.csr.reserved.size() * sizeof(uint64_t));

    if (!get(in, ckpt.guest_base)) return std::nullopt;

    uint64_t nregions = 0;
    if (!get(in, nregions)) return std::nullopt;
    ckpt.memory.reserve(nregions);
    for (uint64_t i = 0; i < nregions; i++) {
        MemoryRegionSnapshot r;
        uint8_t anon = 0;
        uint64_t len = 0;
        if (!get(in, r.addr)) return std::nullopt;
        if (!get(in, r.prot)) return std::nullopt;
        if (!get(in, anon)) return std::nullopt;
        r.anonymous = anon != 0;
        if (!get(in, r.file_offset)) return std::nullopt;
        if (!get_str(in, r.path)) return std::nullopt;
        if (!get(in, len)) return std::nullopt;
        r.data.resize(len);
        if (len) in.read(reinterpret_cast<char*>(r.data.data()),
                         static_cast<std::streamsize>(len));
        if (!in) return std::nullopt;
        ckpt.memory.push_back(std::move(r));
    }

    if (!in.good() && !in.eof()) return std::nullopt;
    return ckpt;
}
