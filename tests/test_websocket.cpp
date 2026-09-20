// WebSocket 协议原语测试。
// 握手向量来自 RFC 6455 §1.3；帧编解码为自构造往返 + 边界。

#include <mfweb/net/websocket/ws.hpp>
#include <mfweb/test/test.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

using mfweb::net::websocket::parse_frame_header;
using mfweb::net::websocket::parse_status;
using mfweb::net::websocket::frame_header;
using mfweb::net::websocket::opcode;

// 构造一个"客户端掩码帧"的字节流（mask key + payload 已掩码）
[[nodiscard]] std::string masked_frame(const char* payload, std::size_t len,
                                       std::uint32_t mask_key, opcode op = opcode::text,
                                       bool fin = true) {
    std::string out;
    const std::uint8_t b0 = static_cast<std::uint8_t>((fin ? 0x80u : 0u) |
                                                      (static_cast<std::uint8_t>(op) & 0x0Fu));
    out.push_back(static_cast<char>(b0));
    out.push_back(static_cast<char>(0x80u | static_cast<std::uint8_t>(len)));  // MASK + len
    out.push_back(static_cast<char>((mask_key >> 24) & 0xFF));
    out.push_back(static_cast<char>((mask_key >> 16) & 0xFF));
    out.push_back(static_cast<char>((mask_key >> 8) & 0xFF));
    out.push_back(static_cast<char>(mask_key & 0xFF));
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t key_byte = static_cast<std::uint8_t>((mask_key >> (8 * (3 - (i % 4)))) & 0xFF);
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key_byte));
    }
    return out;
}

}  // namespace

MFW_TEST(websocket, handshake_accept_rfc_vector) {
    // RFC 6455 §1.3：key "dGhlIHNhbXBsZSBub25jZQ==" →
    // accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    const std::string accept = mfweb::net::websocket::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    MFW_CHECK_EQ(accept, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

MFW_TEST(websocket, parse_small_masked_frame) {
    const std::string raw = masked_frame("Hello", 5, 0x37FA213D);
    frame_header h;
    std::size_t header_size = 0;
    const auto st = parse_frame_header(raw.data(), raw.size(), h, header_size);

    MFW_CHECK_EQ(st, parse_status::ok);
    MFW_CHECK(h.fin);
    MFW_CHECK(h.masked);
    MFW_CHECK_EQ(h.op, opcode::text);
    MFW_CHECK_EQ(h.length, static_cast<std::uint64_t>(5));
    MFW_CHECK_EQ(h.mask_key, static_cast<std::uint32_t>(0x37FA213D));
    MFW_CHECK_EQ(header_size, static_cast<std::size_t>(6));

    // 解掩码后应还原
    char payload[5];
    std::memcpy(payload, raw.data() + header_size, 5);
    mfweb::net::websocket::apply_mask(payload, 5, h.mask_key);
    MFW_CHECK(std::memcmp(payload, "Hello", 5) == 0);
}

MFW_TEST(websocket, parse_16bit_and_64bit_lengths) {
    // 16 位长度
    {
        std::string raw;
        raw.push_back(static_cast<char>(0x81));
        raw.push_back(static_cast<char>(126));
        raw.push_back(static_cast<char>(0x01));
        raw.push_back(static_cast<char>(0x00));  // 256
        frame_header h;
        std::size_t hs = 0;
        const auto st = parse_frame_header(raw.data(), raw.size(), h, hs);
        MFW_CHECK_EQ(st, parse_status::ok);
        MFW_CHECK_EQ(h.length, static_cast<std::uint64_t>(256));
        MFW_CHECK_EQ(hs, static_cast<std::size_t>(4));
    }

    // 64 位长度
    {
        std::string raw;
        raw.push_back(static_cast<char>(0x81));
        raw.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            raw.push_back(static_cast<char>((static_cast<std::uint64_t>(0x00010001) >> (8 * i)) & 0xFF));
        }
        frame_header h;
        std::size_t hs = 0;
        const auto st = parse_frame_header(raw.data(), raw.size(), h, hs);
        MFW_CHECK_EQ(st, parse_status::ok);
        MFW_CHECK_EQ(h.length, static_cast<std::uint64_t>(0x10001));
        MFW_CHECK_EQ(hs, static_cast<std::size_t>(10));
    }
}

MFW_TEST(websocket, parse_rejects_rsv_bits) {
    std::string raw;
    raw.push_back(static_cast<char>(0xC1));  // FIN + RSV1 + text
    raw.push_back(static_cast<char>(0));
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::error);
}

MFW_TEST(websocket, parse_rejects_nonminimal_16bit_encoding) {
    std::string raw;
    raw.push_back(static_cast<char>(0x81));
    raw.push_back(static_cast<char>(126));
    raw.push_back(static_cast<char>(0x00));
    raw.push_back(static_cast<char>(0x05));  // 长度 5 却用 16 位编码 → 非最小
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::error);
}

MFW_TEST(websocket, parse_rejects_fragmented_control_frame) {
    std::string raw;
    raw.push_back(static_cast<char>(0x09));  // 无 FIN 的 ping
    raw.push_back(static_cast<char>(0));
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::error);
}

MFW_TEST(websocket, parse_rejects_oversized_control_payload) {
    std::string raw;
    raw.push_back(static_cast<char>(0x89));  // FIN ping
    raw.push_back(static_cast<char>(126));   // 16 位长度
    raw.push_back(static_cast<char>(0x00));
    raw.push_back(static_cast<char>(0x80));  // 128 > 125
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::error);
}

MFW_TEST(websocket, parse_rejects_invalid_opcode) {
    std::string raw;
    raw.push_back(static_cast<char>(0x83));  // opcode 3（保留）
    raw.push_back(static_cast<char>(0));
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::error);
}

MFW_TEST(websocket, parse_needs_more_when_incomplete) {
    std::string raw;
    raw.push_back(static_cast<char>(0x81));
    frame_header h;
    std::size_t hs = 0;
    MFW_CHECK_EQ(parse_frame_header(raw.data(), raw.size(), h, hs), parse_status::need_more);
}

MFW_TEST(websocket, server_frame_serialization) {
    // 服务端帧：不掩码
    const std::string frame = mfweb::net::websocket::make_frame(true, opcode::text, "hi");
    MFW_CHECK_EQ(frame.size(), static_cast<std::size_t>(4));  // 2 头 + 2 payload
    MFW_CHECK_EQ(static_cast<std::uint8_t>(frame[0]), static_cast<std::uint8_t>(0x81));
    MFW_CHECK_EQ(static_cast<std::uint8_t>(frame[1]), static_cast<std::uint8_t>(0x02));
    MFW_CHECK_EQ(frame[2], 'h');
    MFW_CHECK_EQ(frame[3], 'i');
}

MFW_TEST(websocket, close_payload_roundtrip) {
    const std::string payload =
        mfweb::net::websocket::make_close_payload(mfweb::net::websocket::close_code::protocol_error);
    MFW_CHECK_EQ(payload.size(), static_cast<std::size_t>(2));
    MFW_CHECK_EQ(mfweb::net::websocket::close_code_of(payload),
                 mfweb::net::websocket::close_code::protocol_error);
}
