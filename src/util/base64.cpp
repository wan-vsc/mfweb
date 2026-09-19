#include <mfweb/util/base64.hpp>

namespace mfweb::util {
namespace {

constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

}  // namespace

std::string base64_encode(const void* data, std::size_t len) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
    }

    const std::size_t rest = len - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool base64_decode(std::string_view s, std::vector<std::uint8_t>& out) {
    out.clear();
    if (s.size() % 4 != 0) { return false; }

    std::size_t padding = 0;
    if (!s.empty() && s.back() == '=') {
        ++padding;
        if (s.size() >= 2 && s[s.size() - 2] == '=') { ++padding; }
    }

    out.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int c0 = -1, c1 = -1, c2 = -1, c3 = -1;
        c0 = decode_char(s[i]);
        c1 = decode_char(s[i + 1]);
        if (s[i + 2] != '=') { c2 = decode_char(s[i + 2]); }
        if (s[i + 3] != '=') { c3 = decode_char(s[i + 3]); }

        if (c0 < 0 || c1 < 0 || (c2 < 0 && s[i + 2] != '=') || (c3 < 0 && s[i + 3] != '=')) {
            return false;
        }

        const std::uint32_t v = (static_cast<std::uint32_t>(c0) << 18) |
                                (static_cast<std::uint32_t>(c1) << 12) |
                                (static_cast<std::uint32_t>(c2 < 0 ? 0 : c2) << 6) |
                                static_cast<std::uint32_t>(c3 < 0 ? 0 : c3);
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        if (s[i + 2] != '=') { out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF)); }
        if (s[i + 3] != '=') { out.push_back(static_cast<std::uint8_t>(v & 0xFF)); }
    }
    (void)padding;
    return true;
}

}  // namespace mfweb::util
