#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct RiscvContext {
    static constexpr int NUM_GPR = 32;
    static constexpr int NUM_FPR = 32;

    uint64_t pc = 0;
    std::array<uint64_t, NUM_GPR> gpr = {};
    std::array<uint64_t, NUM_FPR> fpr = {};
    uint32_t fcsr = 0;

    bool serialize(const std::string& path) const;
    static std::optional<RiscvContext> deserialize(const std::string& path);
};

// Machine-level CSRs. User-mode QEMU has no access to these (no privileged
// state, no page tables), so `present` stays false and the fields are zero on
// the QEMU side. They exist so the on-disk format matches a full-system /
// FPGA snapshot (CVA6 M3 dumps mstatus/mepc/satp), keeping one wire layout.
struct CsrState {
    bool     present = false;
    uint64_t mstatus = 0;
    uint64_t mepc    = 0;
    uint64_t satp    = 0;
    std::array<uint64_t, 8> reserved = {};  // headroom for additional CSRs
};

struct MemoryRegionSnapshot {
    // Permission bits, matching /proc/pid/maps perms.
    static constexpr uint32_t PROT_R = 1u << 0;
    static constexpr uint32_t PROT_W = 1u << 1;
    static constexpr uint32_t PROT_X = 1u << 2;

    uint64_t addr = 0;          // guest virtual address (guest_base subtracted)
    uint32_t prot = 0;          // PROT_R | PROT_W | PROT_X
    bool     anonymous = true;  // false => backed by `path` at `file_offset`
    uint64_t file_offset = 0;
    std::string path;           // backing file (empty when anonymous)
    std::vector<uint8_t> data;

    size_t size() const { return data.size(); }
};

struct ExecutionCheckpoint {
    RiscvContext registers;
    CsrState     csr;             // machine CSRs; empty on the user-mode side
    uint64_t     guest_base = 0;  // host_addr = guest_addr + guest_base
    std::vector<MemoryRegionSnapshot> memory;

    // Portable on-disk blob: "RVCKPT01" magic, registers, CSR block,
    // guest_base, then the region table (metadata + bytes). This is the
    // format exchanged with the FPGA backend.
    bool serialize(const std::string& path) const;
    static std::optional<ExecutionCheckpoint> deserialize(const std::string& path);
};
