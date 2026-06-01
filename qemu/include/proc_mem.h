#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

struct MemoryRegion {
    uint64_t start;
    uint64_t end;
    uint64_t offset;   // file offset for file-backed mappings, 0 otherwise
    bool readable;
    bool writable;
    bool executable;
    bool is_private;
    std::string pathname;

    size_t size() const { return end - start; }
};

class ProcMem {
public:
    explicit ProcMem(pid_t pid);
    ~ProcMem();

    ProcMem(const ProcMem&) = delete;
    ProcMem& operator=(const ProcMem&) = delete;

    bool open();
    void close();
    bool is_open() const;

    std::optional<std::vector<uint8_t>> read(uint64_t addr, size_t length);
    bool write(uint64_t addr, const std::vector<uint8_t>& data);

    std::optional<std::vector<MemoryRegion>> regions() const;

private:
    pid_t pid_;
    int mem_fd_ = -1;
};
