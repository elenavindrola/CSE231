#include "binfmt.h"

#include <fstream>
#include <string>

static const std::string STATUS_PATH =
    std::string("/proc/sys/fs/binfmt_misc/") + binfmt::HANDLER_NAME;

// RISC-V 64-bit ELF identification:
//   bytes 0-3:   \x7fELF       (ELF magic)
//   byte  4:     \x02           (64-bit)
//   byte  5:     \x01           (little-endian)
//   byte  6:     \x01           (ELF version)
//   byte  7:     varies         (OS/ABI — masked out)
//   bytes 8-15:  padding
//   bytes 16-17: \x02\x00       (ET_EXEC; mask \xfe catches ET_DYN too)
//   bytes 18-19: \xf3\x00       (EM_RISCV = 243)

static const char* MAGIC =
    "\\x7fELF\\x02\\x01\\x01\\x00"
    "\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00"
    "\\x02\\x00\\xf3\\x00";

static const char* MASK =
    "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00"
    "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff"
    "\\xfe\\xff\\xff\\xff";

bool binfmt::register_handler(const std::string& interpreter_path) {
    unregister_handler();

    std::string entry = std::string(":") + HANDLER_NAME + ":M:0:" + MAGIC +
                        ":" + MASK + ":" + interpreter_path + ":";

    std::ofstream reg(REGISTER_PATH);
    if (!reg) return false;
    reg << entry;
    return reg.good();
}

bool binfmt::unregister_handler() {
    if (!is_registered()) return true;

    std::ofstream ctl(STATUS_PATH);
    if (!ctl) return false;
    ctl << -1;
    return ctl.good();
}

bool binfmt::is_registered() {
    std::ifstream status(STATUS_PATH);
    return status.good();
}
