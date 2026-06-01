#pragma once

#include "execution_context.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class GdbClient {
public:
    explicit GdbClient(const std::string& host = "127.0.0.1",
                       uint16_t port = 1234);
    ~GdbClient();

    GdbClient(const GdbClient&) = delete;
    GdbClient& operator=(const GdbClient&) = delete;

    bool connect(int timeout_ms = 5000);
    void disconnect();
    bool is_connected() const;

    std::optional<std::string> query_halt_reason();
    std::optional<RiscvContext> read_registers();
    bool write_registers(const RiscvContext& ctx);

    std::optional<std::vector<uint8_t>> read_memory(uint64_t addr,
                                                    size_t length);
    bool write_memory(uint64_t addr, const std::vector<uint8_t>& data);

    bool continue_execution();
    bool interrupt();
    bool wait_stop(int timeout_ms = 5000);
    bool detach();

private:
    std::string host_;
    uint16_t port_;
    int sock_fd_ = -1;
    std::string recv_buf_;

    static constexpr size_t MEM_CHUNK_BYTES = 1024;

    bool fill_recv_buf(int timeout_ms);
    bool send_packet(const std::string& data);
    std::optional<std::string> recv_packet(int timeout_ms = 5000);

    static uint8_t checksum(const std::string& data);
    static std::string hex_encode(const uint8_t* data, size_t len);
    static std::vector<uint8_t> hex_decode(const std::string& hex);
};
