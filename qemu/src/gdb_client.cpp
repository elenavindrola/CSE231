#include "gdb_client.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

GdbClient::GdbClient(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

GdbClient::~GdbClient() {
    if (sock_fd_ >= 0) disconnect();
}

bool GdbClient::connect(int timeout_ms) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0) return false;

        if (::connect(sock_fd_, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            int one = 1;
            setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return true;
        }

        close(sock_fd_);
        sock_fd_ = -1;
        usleep(50'000);
    }
    return false;
}

void GdbClient::disconnect() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    recv_buf_.clear();
}

bool GdbClient::is_connected() const { return sock_fd_ >= 0; }

uint8_t GdbClient::checksum(const std::string& data) {
    uint8_t sum = 0;
    for (char c : data) sum += static_cast<uint8_t>(c);
    return sum;
}

bool GdbClient::send_packet(const std::string& data) {
    std::ostringstream pkt;
    pkt << '$' << data << '#' << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(checksum(data));

    std::string s = pkt.str();
    return send(sock_fd_, s.c_str(), s.size(), 0) ==
           static_cast<ssize_t>(s.size());
}

bool GdbClient::fill_recv_buf(int timeout_ms) {
    pollfd pfd{sock_fd_, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) return false;

    char tmp[4096];
    ssize_t n = recv(sock_fd_, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    recv_buf_.append(tmp, static_cast<size_t>(n));
    return true;
}

std::optional<std::string> GdbClient::recv_packet(int timeout_ms) {
    // Scan for '$' — skip ack chars (+/-)
    while (true) {
        size_t dollar = std::string::npos;
        for (size_t i = 0; i < recv_buf_.size(); i++) {
            char c = recv_buf_[i];
            if (c == '$') { dollar = i; break; }
            if (c != '+' && c != '-') {
                recv_buf_.erase(0, i + 1);
                return std::nullopt;
            }
        }

        if (dollar != std::string::npos) {
            recv_buf_.erase(0, dollar);
            break;
        }
        recv_buf_.clear();
        if (!fill_recv_buf(timeout_ms)) return std::nullopt;
    }

    // Now recv_buf_ starts with '$'. Find '#' + 2 checksum chars.
    while (true) {
        size_t hash = recv_buf_.find('#', 1);
        if (hash != std::string::npos && hash + 2 < recv_buf_.size()) {
            std::string data = recv_buf_.substr(1, hash - 1);
            recv_buf_.erase(0, hash + 3);

            char ack = '+';
            ::send(sock_fd_, &ack, 1, 0);
            return data;
        }
        if (!fill_recv_buf(timeout_ms)) return std::nullopt;
    }
}

std::string GdbClient::hex_encode(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++)
        ss << std::setw(2) << static_cast<int>(data[i]);
    return ss.str();
}

std::vector<uint8_t> GdbClient::hex_decode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back(
            static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

// ---- RISC-V 64 GDB register layout (qemu-riscv64) ----
// x0..x31  : 32 × 8 bytes = 256 bytes
// pc       :  1 × 8 bytes =   8 bytes
// f0..f31  : 32 × 8 bytes = 256 bytes  (when F/D present)
// fcsr     :  1 × 4 bytes =   4 bytes
// All little-endian.

static uint64_t le64_decode(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void le64_encode(uint64_t v, uint8_t* p) {
    for (int i = 0; i < 8; i++) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}

static uint32_t le32_decode(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static void le32_encode(uint32_t v, uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}

static constexpr size_t GPR_BYTES = 32 * 8;
static constexpr size_t PC_BYTES = 8;
static constexpr size_t MIN_REG_BYTES = GPR_BYTES + PC_BYTES;
static constexpr int GDB_FPR_BASE = 33;
static constexpr int GDB_FCSR = 101;

std::optional<std::string> GdbClient::query_halt_reason() {
    if (!send_packet("?")) return std::nullopt;
    return recv_packet();
}

std::optional<RiscvContext> GdbClient::read_registers() {
    // GPRs + PC via bulk 'g' packet
    if (!send_packet("g")) return std::nullopt;
    auto resp = recv_packet();
    if (!resp) return std::nullopt;

    auto bytes = hex_decode(*resp);
    if (bytes.size() < MIN_REG_BYTES) return std::nullopt;

    RiscvContext ctx{};
    for (int i = 0; i < 32; i++)
        ctx.gpr[i] = le64_decode(&bytes[i * 8]);
    ctx.pc = le64_decode(&bytes[GPR_BYTES]);

    // QEMU's 'g' response only includes the base integer set — FPRs
    // and fcsr must be read individually via 'p' packets.
    for (int i = 0; i < 32; i++) {
        std::ostringstream cmd;
        cmd << "p" << std::hex << (GDB_FPR_BASE + i);
        if (!send_packet(cmd.str())) return std::nullopt;
        resp = recv_packet();
        if (!resp || resp->empty() || (*resp)[0] == 'E') return std::nullopt;
        auto fpr = hex_decode(*resp);
        if (fpr.size() < 8) return std::nullopt;
        ctx.fpr[i] = le64_decode(fpr.data());
    }

    std::ostringstream fcsr_cmd;
    fcsr_cmd << "p" << std::hex << GDB_FCSR;
    if (!send_packet(fcsr_cmd.str())) return std::nullopt;
    resp = recv_packet();
    if (!resp || resp->empty() || (*resp)[0] == 'E') return std::nullopt;
    auto fcsr_bytes = hex_decode(*resp);
    if (fcsr_bytes.size() < 4) return std::nullopt;
    ctx.fcsr = le32_decode(fcsr_bytes.data());

    return ctx;
}

bool GdbClient::write_registers(const RiscvContext& ctx) {
    // GPRs + PC via bulk G packet
    std::vector<uint8_t> gpr_bytes(GPR_BYTES + PC_BYTES);
    for (int i = 0; i < 32; i++)
        le64_encode(ctx.gpr[i], &gpr_bytes[i * 8]);
    le64_encode(ctx.pc, &gpr_bytes[GPR_BYTES]);

    if (!send_packet("G" + hex_encode(gpr_bytes.data(), gpr_bytes.size())))
        return false;
    auto resp = recv_packet();
    if (!resp || *resp != "OK") return false;

    // QEMU's G handler only processes the base integer set — FPRs must be
    // written individually via P packets.
    for (int i = 0; i < 32; i++) {
        uint8_t buf[8];
        le64_encode(ctx.fpr[i], buf);
        std::ostringstream cmd;
        cmd << "P" << std::hex << (GDB_FPR_BASE + i) << "=" << hex_encode(buf, 8);
        if (!send_packet(cmd.str())) return false;
        resp = recv_packet();
        if (!resp || *resp != "OK") return false;
    }

    // fcsr — GDB register 101 (0x65), exposed as 64-bit by QEMU
    uint8_t fcsr_buf[8] = {};
    le64_encode(static_cast<uint64_t>(ctx.fcsr), fcsr_buf);
    std::ostringstream fcsr_cmd;
    fcsr_cmd << "P" << std::hex << GDB_FCSR << "=" << hex_encode(fcsr_buf, 8);
    if (!send_packet(fcsr_cmd.str())) return false;
    resp = recv_packet();
    return resp && *resp == "OK";
}

std::optional<std::vector<uint8_t>> GdbClient::read_memory(uint64_t addr,
                                                           size_t length) {
    std::vector<uint8_t> result;
    result.reserve(length);

    while (result.size() < length) {
        size_t remaining = length - result.size();
        size_t chunk = std::min(remaining, MEM_CHUNK_BYTES);

        std::ostringstream cmd;
        cmd << "m" << std::hex << (addr + result.size()) << "," << chunk;

        if (!send_packet(cmd.str())) return std::nullopt;
        auto resp = recv_packet();
        if (!resp || resp->empty() || (*resp)[0] == 'E') return std::nullopt;

        auto bytes = hex_decode(*resp);
        if (bytes.size() != chunk) return std::nullopt;

        result.insert(result.end(), bytes.begin(), bytes.end());
    }

    return result;
}

bool GdbClient::write_memory(uint64_t addr,
                             const std::vector<uint8_t>& data) {
    size_t offset = 0;

    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t chunk = std::min(remaining, MEM_CHUNK_BYTES);

        std::ostringstream cmd;
        cmd << "M" << std::hex << (addr + offset) << "," << chunk << ":";
        cmd << hex_encode(&data[offset], chunk);

        if (!send_packet(cmd.str())) return false;
        auto resp = recv_packet();
        if (!resp || *resp != "OK") return false;

        offset += chunk;
    }

    return true;
}

bool GdbClient::continue_execution() {
    if (!send_packet("c")) return false;
    // Drain the ack so it doesn't interfere with later recv_packet calls.
    // QEMU sends '+' immediately; the stop reply comes later when the
    // guest is interrupted.
    fill_recv_buf(1000);
    // Discard any '+'/'-' acks that arrived
    size_t skip = 0;
    while (skip < recv_buf_.size() &&
           (recv_buf_[skip] == '+' || recv_buf_[skip] == '-'))
        skip++;
    if (skip > 0) recv_buf_.erase(0, skip);
    return true;
}

bool GdbClient::interrupt() {
    char brk = 0x03;
    if (::send(sock_fd_, &brk, 1, 0) != 1) return false;
    auto reply = recv_packet(5000);
    return reply.has_value();
}

bool GdbClient::wait_stop(int timeout_ms) {
    auto reply = recv_packet(timeout_ms);
    return reply.has_value();
}

bool GdbClient::detach() {
    if (!send_packet("D")) return false;
    auto resp = recv_packet();
    disconnect();
    return resp && (*resp == "OK" || resp->empty());
}
