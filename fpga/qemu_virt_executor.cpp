// qemu_virt_executor.cpp — see qemu_virt_executor.h.

#include "qemu_virt_executor.h"

#include <limits.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

bool executable(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

// PATH lookup, execvp-style.
std::string find_in_path(const char* name) {
    const char* path = getenv("PATH");
    if (!path) return {};
    for (const char* s = path; ; ) {
        const char* e = strchr(s, ':');
        std::string dir = e ? std::string(s, e - s) : std::string(s);
        if (!dir.empty()) {
            fs::path cand = fs::path(dir) / name;
            if (executable(cand)) return cand.string();
        }
        if (!e) break;
        s = e + 1;
    }
    return {};
}

// The in-repo Xilinx-QEMU wrapper, located relative to this executable
// (<repo>/build/dispatcher -> <repo>/cva6_fk33/sw/qemu-virt.sh).
std::string find_repo_wrapper() {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';
    fs::path cand = fs::path(exe).parent_path().parent_path()
                    / "cva6_fk33" / "sw" / "qemu-virt.sh";
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(cand, ec);
    if (!ec && executable(canon)) return canon.string();
    return {};
}

std::string resolve_qemu() {
    if (const char* env = getenv("QEMU_SYSTEM")) return env;
    if (auto p = find_in_path("qemu-system-riscv64"); !p.empty()) return p;
    return find_repo_wrapper();
}

}  // namespace

QemuVirtExecutor::QemuVirtExecutor() : qemu_(resolve_qemu()) {
    // Replace QemuConfig's cwd-relative default ("./qemu-virt.sh") with the
    // resolved binary; the remaining virt-machine flags stay as-is.
    if (!qemu_.empty()) config_.argv[0] = qemu_;
}

bool QemuVirtExecutor::precheck(const std::string& binary) {
    if (qemu_.empty()) {
        fprintf(stderr, "[qemu-virt] no qemu-system-riscv64 found "
                "(set QEMU_SYSTEM or install qemu-system-misc)\n");
        return false;
    }
    if (access(binary.c_str(), R_OK) != 0) {
        fprintf(stderr, "[qemu-virt] cannot read %s\n", binary.c_str());
        return false;
    }
    return true;
}

std::unique_ptr<fpgaexec::Backend> QemuVirtExecutor::make_backend() {
    fprintf(stderr, "[qemu-virt] using %s\n", qemu_.c_str());
    return std::make_unique<fpgaexec::QemuBackend>(config_);
}
