#include <mfweb/net/websocket/ws.hpp>

#include <mfweb/util/base64.hpp>
#include <mfweb/util/sha1.hpp>

namespace mfweb::net::websocket {
namespace {

[[nodiscard]] std::uint32_t read_u32(const char* p) noexcept {
    const auto b = [p](std::size_t i) { return static_cast<std::uint8_t>(p[i]); };
    return (static_cast<std::uint32_t>(b(0)) << 24) | (static_cast<std::uint32_t>(b(1)) << 16) |
           (static_cast<std::uint32_t>(b(2)) << 8) | static_cast<std::uint32_t>(b(3));
}

}  // namespace

std::string compute_accept_key(std::string_view sec_websocket_key) {
    std::string material;
    material.reserve(sec_websocket_key.size() + kMagicGuid.size());
    material.append(sec_websocket_key);
    material.append(kMagicGuid);
    const auto digest = mfweb::util::sha1(material.data(), material.size());
    return mfweb::util::base64_encode(digest.data(), digest.size());
}

parse_status parse_frame_header(const char* data, std::size_t len, frame_header& out,
                                std::size_t& header_size) noexcept {
    if (len < 2) { return parse_status::need_more; }

    const std::uint8_t b0 = static_cast<std::uint8_t>(data[0]);
    const std::uint8_t b1 = static_cast<std::uint8_t>(data[1]);

    out.fin = (b0 & 0x80u) != 0;
    if ((b0 & 0x70u) != 0) { return parse_status::error; }  // RSV1..3 必须为 0
    out.op = static_cast<opcode>(b0 & 0x0Fu);
    out.masked = (b1 & 0x80u) != 0;
    const std::uint64_t len7 = b1 & 0x7Fu;

    std::size_t pos = 2;
    std::uint64_t length = len7;
    if (len7 == 126) {
        if (len < 4) { return parse_status::need_more; }
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2])) << 8) |
                 static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[3]));
        if (length < 126) { return parse_status::error; }  // 非最小编码
        pos = 4;
    } else if (len7 == 127) {
        if (len < 10) { return parse_status::need_more; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2 + i]));
        }
        if ((v & (1ull << 63)) != 0) { return parse_status::error; }  // MSB 必须为 0
        if (v <= 0xFFFF) { return parse_status::error; }               // 非最小编码
        length = v;
        pos = 10;
    }
    out.length = length;

    out.mask_key = 0;
    if (out.masked) {
        if (len < pos + 4) { return parse_status::need_more; }
        out.mask_key = read_u32(data + pos);
        pos += 4;
    }

    // 控制帧约束（RFC 6455 §5.5）：FIN 且 payload ≤ 125
    if (is_control(out.op)) {
        if (!out.fin) { return parse_status::error; }
        if (length > 125) { return parse_status::error; }
    }

    // opcode 合法性
    switch (out.op) {
        case opcode::continuation:
        case opcode::text:
        case opcode::binary:
        case opcode::close:
        case opcode::ping:
        case opcode::pong:
            break;
        default:
            return parse_status::error;
    }

    header_size = pos;
    return parse_status::ok;
}

void append_frame_header(std::string& out, bool fin, opcode op, std::uint64_t length) {
    const std::uint8_t b0 = static_cast<std::uint8_t>((fin ? 0x80u : 0u) |
                                                      (static_cast<std::uint8_t>(op) & 0x0Fu));
    out.push_back(static_cast<char>(b0));

    if (length < 126) {
        out.push_back(static_cast<char>(length));
    } else if (length <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((length >> 8) & 0xFF));
        out.push_back(static_cast<char>(length & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((length >> (8 * i)) & 0xFF));
        }
    }
}

std::string make_frame(bool fin, opcode op, std::string_view payload) {
    std::string out;
    append_frame_header(out, fin, op, payload.size());
    out.append(payload.data(), payload.size());
    return out;
}

void apply_mask(char* data, std::size_t len, std::uint32_t mask_key) noexcept {
    const std::uint8_t key[4] = {
        static_cast<std::uint8_t>((mask_key >> 24) & 0xFF),
        static_cast<std::uint8_t>((mask_key >> 16) & 0xFF),
        static_cast<std::uint8_t>((mask_key >> 8) & 0xFF),
        static_cast<std::uint8_t>(mask_key & 0xFF),
    };
    for (std::size_t i = 0; i < len; ++i) {
        data[i] = static_cast<char>(static_cast<std::uint8_t>(data[i]) ^ key[i % 4]);
    }
}

std::string make_close_payload(close_code code) {
    std::string out;
    out.push_back(static_cast<char>((static_cast<std::uint16_t>(code) >> 8) & 0xFF));
    out.push_back(static_cast<char>(static_cast<std::uint16_t>(code) & 0xFF));
    return out;
}

close_code close_code_of(std::string_view payload) noexcept {
    if (payload.size() < 2) { return close_code::normal; }
    const std::uint16_t v =
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0])) << 8) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1]));
    return static_cast<close_code>(v);
}

}  // namespace mfweb::net::websocket
