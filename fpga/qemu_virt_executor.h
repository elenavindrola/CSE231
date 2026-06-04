// qemu_virt_executor.h — run a baremetal ELF under qemu-system-riscv64 -machine virt.
//
// The QEMU twin of FpgaExecutor: the FK33 bitstream mirrors the QEMU virt
// device map, so the SAME baremetal ELF runs on either. This is NOT the
// dispatcher's user-mode QemuExecutor (qemu-riscv64), which runs Linux
// user-space binaries — a baremetal ELF dies silently there (its UART MMIO
// stores hit unmapped memory). The dispatcher picks by ELF entry point.
//
// The qemu-system binary is resolved at construction, in order:
//   1. $QEMU_SYSTEM (explicit override, may be a wrapper script)
//   2. qemu-system-riscv64 on $PATH
//   3. <repo>/cva6_fk33/sw/qemu-virt.sh next to the dispatcher's build dir
//      (the Xilinx-bundled QEMU wrapper, for hosts with no standalone QEMU)

#pragma once

#include "fpgaexec_adapter.h"

class QemuVirtExecutor : public FpgaexecAdapter {
public:
    QemuVirtExecutor();

protected:
    bool precheck(const std::string& binary) override;
    std::unique_ptr<fpgaexec::Backend> make_backend() override;

private:
    fpgaexec::QemuConfig config_;
    std::string qemu_;  // resolved binary; empty = none found
};
