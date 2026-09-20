#pragma once

// mfweb::net::websocket —— WebSocket 协议原语（RFC 6455）。
//
// 覆盖：
//   * 握手：Sec-WebSocket-Accept = base64(SHA1(key + magic GUID))；
//   * 帧头解析/序列化：FIN/RSV/opcode、MASK、7/16/64 位长度（强制最小编码）；
//   * 掩码（客户端→服务端必须掩码，服务端→客户端不得掩码）；
//   * 控制帧规则：控制帧必须 FIN 且 payload ≤ 125 字节，不得分片。
//
// 校验失败一律按 RFC 6455 §7 归类为协议错误（1002）——连接必须关闭，不能继续解析。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mfweb::net::websocket {

inline constexpr std::string_view kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 握手应答计算（RFC 6455 §1.3）
[[nodiscard]] std::string compute_accept_key(std::string_view sec_websocket_key);

enum class opcode : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

[[nodiscard]] inline bool is_control(opcode op) noexcept {
    return static_cast<std::uint8_t>(op) >= 0x8;
}

enum class close_code : std::uint16_t {
    normal = 1000,
    protocol_error = 1002,
    invalid_payload = 1007,
    message_too_big = 1009,
};

enum class parse_status { ok, need_more, error };

struct frame_header {
    bool fin = true;
    bool masked = false;
    opcode op = opcode::text;
    std::uint32_t mask_key = 0;
    std::uint64_t length = 0;
};

// 解析帧头。data/len 是当前可用的全部字节。
// ok 时 header_size 为帧头字节数，调用方还需确保有 header_size + length 字节的 payload。
[[nodiscard]] parse_status parse_frame_header(const char* data, std::size_t len,
                                              frame_header& out, std::size_t& header_size) noexcept;

// 序列化帧头（服务器→客户端，不掩码）
void append_frame_header(std::string& out, bool fin, opcode op, std::uint64_t length);

// 帧的完整编码（头 + payload，不掩码）
[[nodiscard]] std::string make_frame(bool fin, opcode op, std::string_view payload);

// 掩码应用/解除（XOR 原地）
void apply_mask(char* data, std::size_t len, std::uint32_t mask_key) noexcept;

// close 帧 payload：2 字节网络序关闭码
[[nodiscard]] std::string make_close_payload(close_code code);

// 从 close payload 读关闭码
[[nodiscard]] close_code close_code_of(std::string_view payload) noexcept;

}  // namespace mfweb::net::websocket
