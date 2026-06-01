#include "gdb_client.h"

#include <arpa/inet.h>
#include <cstring>
#include <iomanip>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

static uint8_t gdb_checksum(const std::string& data) {
    uint8_t sum = 0;
    for (char c : data) sum += static_cast<uint8_t>(c);
    return sum;
}

static std::string gdb_wrap(const std::string& data) {
    std::ostringstream pkt;
    pkt << '$' << data << '#' << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<int>(gdb_checksum(data));
    return pkt.str();
}

static std::string recv_packet_raw(int fd) {
    std::string buf;
    char c;
    while (recv(fd, &c, 1, 0) == 1) {
        buf += c;
        if (buf.size() >= 4 && buf[buf.size() - 3] == '#') break;
    }
    char ack = '+';
    ::send(fd, &ack, 1, 0);
    return buf;
}

static std::string hex_encode(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++)
        ss << std::setw(2) << static_cast<int>(data[i]);
    return ss.str();
}

static void le64_encode(uint64_t v, uint8_t* p) {
    for (int i = 0; i < 8; i++) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}

static void le32_encode(uint32_t v, uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        p[i] = v & 0xff;
        v >>= 8;
    }
}

static std::vector<uint8_t> hex_decode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back(
            static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

static uint64_t le64_decode(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t le32_decode(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static std::string extract_payload(const std::string& pkt) {
    size_t dollar = pkt.find('$');
    size_t hash = pkt.rfind('#');
    if (dollar == std::string::npos || hash == std::string::npos) return "";
    return pkt.substr(dollar + 1, hash - dollar - 1);
}

class MockGdbServer {
public:
    uint16_t port = 0;

    MockGdbServer() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listen_fd_, 1);

        socklen_t len = sizeof(addr);
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }

    ~MockGdbServer() {
        if (client_fd_ >= 0) close(client_fd_);
        if (listen_fd_ >= 0) close(listen_fd_);
    }

    bool accept_client() {
        client_fd_ = accept(listen_fd_, nullptr, nullptr);
        return client_fd_ >= 0;
    }

    std::string recv_packet() { return recv_packet_raw(client_fd_); }

    void send_packet(const std::string& data) {
        std::string pkt = gdb_wrap(data);
        ::send(client_fd_, pkt.c_str(), pkt.size(), 0);
    }

    void send_ack() {
        char ack = '+';
        ::send(client_fd_, &ack, 1, 0);
    }

    int client_fd() const { return client_fd_; }

private:
    int listen_fd_ = -1;
    int client_fd_ = -1;
};

class GdbClientTest : public ::testing::Test {
protected:
    std::unique_ptr<MockGdbServer> server;
    std::unique_ptr<GdbClient> client;

    void SetUp() override {
        server = std::make_unique<MockGdbServer>();
        client = std::make_unique<GdbClient>("127.0.0.1", server->port);
    }

    void connect_pair() {
        std::thread accept_thread([this] { server->accept_client(); });
        ASSERT_TRUE(client->connect(2000));
        accept_thread.join();
    }
};

TEST_F(GdbClientTest, ConnectDisconnect) {
    connect_pair();
    EXPECT_TRUE(client->is_connected());
    client->disconnect();
    EXPECT_FALSE(client->is_connected());
}

TEST_F(GdbClientTest, ConnectTimeout) {
    // No server accept — should timeout
    MockGdbServer dead_server;
    close(dead_server.port); // port is valid but nobody accepts
    GdbClient bad_client("127.0.0.1", 1); // port 1 — nothing listening
    EXPECT_FALSE(bad_client.connect(200));
}

TEST_F(GdbClientTest, QueryHaltReason) {
    connect_pair();

    std::thread server_thread([this] {
        auto pkt = server->recv_packet();
        EXPECT_NE(pkt.find("?"), std::string::npos);
        server->send_ack();
        server->send_packet("S05");
    });

    auto reason = client->query_halt_reason();
    server_thread.join();

    ASSERT_TRUE(reason.has_value());
    EXPECT_EQ(*reason, "S05");
}

TEST_F(GdbClientTest, ReadRegisters) {
    connect_pair();

    RiscvContext expected{};
    expected.pc = 0x0000000080001234;
    for (int i = 0; i < 32; i++) expected.gpr[i] = 0x100 + i;
    for (int i = 0; i < 32; i++) expected.fpr[i] = 0x200 + i;
    expected.fcsr = 0xAB;

    std::thread server_thread([&] {
        // 1) 'g' — respond with GPRs + PC only (264 bytes)
        {
            auto pkt = server->recv_packet();
            EXPECT_NE(pkt.find("g"), std::string::npos);
            server->send_ack();

            std::vector<uint8_t> blob(32 * 8 + 8);
            for (int i = 0; i < 32; i++)
                le64_encode(expected.gpr[i], &blob[i * 8]);
            le64_encode(expected.pc, &blob[32 * 8]);
            server->send_packet(hex_encode(blob.data(), blob.size()));
        }

        // 2) 'p' packets for each FPR (registers 0x21..0x40)
        for (int i = 0; i < 32; i++) {
            std::string payload = extract_payload(server->recv_packet());
            std::ostringstream exp_cmd;
            exp_cmd << "p" << std::hex << (33 + i);
            EXPECT_EQ(payload, exp_cmd.str()) << "fpr[" << i << "] query";
            server->send_ack();

            uint8_t buf[8];
            le64_encode(expected.fpr[i], buf);
            server->send_packet(hex_encode(buf, 8));
        }

        // 3) 'p' for fcsr (register 101 = 0x65)
        {
            std::string payload = extract_payload(server->recv_packet());
            EXPECT_EQ(payload, "p65") << "fcsr query";
            server->send_ack();

            uint8_t buf[8] = {};
            le64_encode(static_cast<uint64_t>(expected.fcsr), buf);
            server->send_packet(hex_encode(buf, 8));
        }
    });

    auto ctx = client->read_registers();
    server_thread.join();

    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->pc, expected.pc);
    EXPECT_EQ(ctx->fcsr, expected.fcsr);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(ctx->gpr[i], expected.gpr[i]) << "gpr[" << i << "]";
        EXPECT_EQ(ctx->fpr[i], expected.fpr[i]) << "fpr[" << i << "]";
    }
}

TEST_F(GdbClientTest, WriteRegisters) {
    connect_pair();

    RiscvContext ctx{};
    ctx.pc = 0xCAFE;
    for (int i = 0; i < 32; i++) ctx.gpr[i] = 0x1000 + i;
    for (int i = 0; i < 32; i++) ctx.fpr[i] = 0x2000 + i;
    ctx.fcsr = 0xAB;

    std::thread server_thread([&] {
        // 1) G packet with GPRs + PC
        {
            std::string payload = extract_payload(server->recv_packet());
            ASSERT_GT(payload.size(), 1u);
            EXPECT_EQ(payload[0], 'G');
            auto bytes = hex_decode(payload.substr(1));
            ASSERT_EQ(bytes.size(), 32u * 8 + 8) << "G packet should contain GPRs + PC only";

            for (int i = 0; i < 32; i++)
                EXPECT_EQ(le64_decode(&bytes[i * 8]), ctx.gpr[i]) << "gpr[" << i << "]";
            EXPECT_EQ(le64_decode(&bytes[32 * 8]), ctx.pc) << "pc";

            server->send_ack();
            server->send_packet("OK");
        }

        // 2) P packets for each FPR (registers 0x21..0x40)
        for (int i = 0; i < 32; i++) {
            std::string payload = extract_payload(server->recv_packet());

            std::ostringstream expected_prefix;
            expected_prefix << "P" << std::hex << (33 + i) << "=";
            EXPECT_EQ(payload.substr(0, expected_prefix.str().size()),
                      expected_prefix.str()) << "fpr[" << i << "] prefix";

            auto bytes = hex_decode(payload.substr(expected_prefix.str().size()));
            ASSERT_EQ(bytes.size(), 8u) << "fpr[" << i << "]";
            EXPECT_EQ(le64_decode(bytes.data()), ctx.fpr[i]) << "fpr[" << i << "]";

            server->send_ack();
            server->send_packet("OK");
        }

        // 3) P packet for fcsr (register 101 = 0x65, 64-bit)
        {
            std::string payload = extract_payload(server->recv_packet());
            EXPECT_EQ(payload.substr(0, 4), "P65=") << "fcsr prefix";
            auto bytes = hex_decode(payload.substr(4));
            ASSERT_EQ(bytes.size(), 8u);
            EXPECT_EQ(le64_decode(bytes.data()), static_cast<uint64_t>(ctx.fcsr)) << "fcsr";

            server->send_ack();
            server->send_packet("OK");
        }
    });

    bool ok = client->write_registers(ctx);
    server_thread.join();
    EXPECT_TRUE(ok);
}

TEST_F(GdbClientTest, ReadMemory) {
    connect_pair();

    std::vector<uint8_t> expected_data = {0xDE, 0xAD, 0xBE, 0xEF};

    std::thread server_thread([&] {
        auto pkt = server->recv_packet();
        server->send_ack();
        server->send_packet(hex_encode(expected_data.data(), expected_data.size()));
    });

    auto result = client->read_memory(0x1000, 4);
    server_thread.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, expected_data);
}

TEST_F(GdbClientTest, WriteMemory) {
    connect_pair();

    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};

    std::thread server_thread([&] {
        auto pkt = server->recv_packet();

        size_t dollar = pkt.find('$');
        size_t hash = pkt.rfind('#');
        ASSERT_NE(dollar, std::string::npos);
        ASSERT_NE(hash, std::string::npos);
        std::string payload = pkt.substr(dollar + 1, hash - dollar - 1);
        EXPECT_EQ(payload[0], 'M');

        size_t colon_pos = payload.find(':');
        ASSERT_NE(colon_pos, std::string::npos);
        auto decoded = hex_decode(payload.substr(colon_pos + 1));
        EXPECT_EQ(decoded, data);

        server->send_ack();
        server->send_packet("OK");
    });

    bool ok = client->write_memory(0x2000, data);
    server_thread.join();
    EXPECT_TRUE(ok);
}

TEST_F(GdbClientTest, WriteMemoryError) {
    connect_pair();

    std::vector<uint8_t> data = {0x01};

    std::thread server_thread([&] {
        server->recv_packet();
        server->send_ack();
        server->send_packet("E14");
    });

    bool ok = client->write_memory(0x2000, data);
    server_thread.join();
    EXPECT_FALSE(ok);
}

TEST_F(GdbClientTest, ContinueExecution) {
    connect_pair();

    std::thread server_thread([&] {
        auto pkt = server->recv_packet();
        EXPECT_NE(pkt.find("c"), std::string::npos);
        server->send_ack();
    });

    bool ok = client->continue_execution();
    server_thread.join();
    EXPECT_TRUE(ok);
}

TEST_F(GdbClientTest, Detach) {
    connect_pair();

    std::thread server_thread([&] {
        auto pkt = server->recv_packet();
        EXPECT_NE(pkt.find("D"), std::string::npos);
        server->send_ack();
        server->send_packet("OK");
    });

    bool ok = client->detach();
    server_thread.join();
    EXPECT_TRUE(ok);
    EXPECT_FALSE(client->is_connected());
}
