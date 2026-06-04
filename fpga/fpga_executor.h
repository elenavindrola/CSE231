// fpga_executor.h — run a baremetal ELF on the FK33 card.
//
// FpgaexecAdapter over fpgaexec::FpgaBackend (DMA the ELF over XDMA, release
// CVA6 from reset, poll the finisher/UART). launch() fails fast — before
// touching the card — when the binary is not a baremetal ELF the bootrom can
// run (entry != 0x80000000) or the XDMA char devices are absent, so the
// dispatcher can fall back to QEMU instead of timing out against missing
// hardware.

#pragma once

#include "fpgaexec_adapter.h"

class FpgaExecutor : public FpgaexecAdapter {
public:
    explicit FpgaExecutor(fpgaexec::FpgaConfig config = {})
        : config_(std::move(config)) {}

protected:
    bool precheck(const std::string& binary) override;
    std::unique_ptr<fpgaexec::Backend> make_backend() override;

private:
    fpgaexec::FpgaConfig config_;
};
