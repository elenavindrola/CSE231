#include "proc_mem.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

ProcMem::ProcMem(pid_t pid) : pid_(pid) {}

ProcMem::~ProcMem() { close(); }

bool ProcMem::open() {
    if (mem_fd_ >= 0) return true;

    std::string path = "/proc/" + std::to_string(pid_) + "/mem";
    mem_fd_ = ::open(path.c_str(), O_RDWR);
    if (mem_fd_ < 0)
        mem_fd_ = ::open(path.c_str(), O_RDONLY);
    return mem_fd_ >= 0;
}

void ProcMem::close() {
    if (mem_fd_ >= 0) {
        ::close(mem_fd_);
        mem_fd_ = -1;
    }
}

bool ProcMem::is_open() const { return mem_fd_ >= 0; }

std::optional<std::vector<uint8_t>> ProcMem::read(uint64_t addr,
                                                   size_t length) {
    if (mem_fd_ < 0) return std::nullopt;

    std::vector<uint8_t> buf(length);
    size_t total = 0;

    while (total < length) {
        ssize_t n = pread(mem_fd_, buf.data() + total, length - total,
                          static_cast<off_t>(addr + total));
        if (n <= 0) return std::nullopt;
        total += static_cast<size_t>(n);
    }

    return buf;
}

bool ProcMem::write(uint64_t addr, const std::vector<uint8_t>& data) {
    if (mem_fd_ < 0) return false;

    size_t total = 0;

    while (total < data.size()) {
        ssize_t n = pwrite(mem_fd_, data.data() + total, data.size() - total,
                           static_cast<off_t>(addr + total));
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }

    return true;
}

std::optional<std::vector<MemoryRegion>> ProcMem::regions() const {
    std::string path = "/proc/" + std::to_string(pid_) + "/maps";
    std::ifstream maps(path);
    if (!maps) return std::nullopt;

    std::vector<MemoryRegion> result;
    std::string line;

    while (std::getline(maps, line)) {
        MemoryRegion region{};
        char perms[5] = {};
        uint64_t offset, dev_major, dev_minor, inode;

        int consumed = 0;
        int matched = std::sscanf(
            line.c_str(), "%lx-%lx %4s %lx %lx:%lx %lu%n",
            &region.start, &region.end, perms, &offset,
            &dev_major, &dev_minor, &inode, &consumed);

        if (matched < 7) continue;

        region.offset = offset;
        region.readable = perms[0] == 'r';
        region.writable = perms[1] == 'w';
        region.executable = perms[2] == 'x';
        region.is_private = perms[3] == 'p';

        const char* rest = line.c_str() + consumed;
        while (*rest == ' ') rest++;
        if (*rest) region.pathname = rest;

        result.push_back(std::move(region));
    }

    return result;
}
