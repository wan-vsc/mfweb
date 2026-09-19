#include <mfweb/util/sha1.hpp>

#include <cstring>

namespace mfweb::util {
namespace {

[[nodiscard]] inline std::uint32_t rotl32(std::uint32_t x, unsigned n) noexcept {
    return (x << n) | (x >> (32 - n));
}

}  // namespace

std::array<std::uint8_t, 20> sha1(const void* data, std::size_t len) noexcept {
    std::array<std::uint8_t, 20> digest{};
    std::uint32_t h0 = 0x67452301u;
    std::uint32_t h1 = 0xEFCDAB89u;
    std::uint32_t h2 = 0x98BADCFEu;
    std::uint32_t h3 = 0x10325476u;
    std::uint32_t h4 = 0xC3D2E1F0u;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;

    const auto process_block = [&h0, &h1, &h2, &h3, &h4](const std::uint8_t* block) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    std::size_t i = 0;
    for (; i + 64 <= len; i += 64) { process_block(bytes + i); }

    // 填充：0x80 + 0x00... + 64 位原始位长
    std::uint8_t tail[128]{};
    const std::size_t remaining = len - i;
    if (remaining != 0) { std::memcpy(tail, bytes + i, remaining); }
    tail[remaining] = 0x80;
    const std::size_t tail_len = (remaining + 1 <= 56) ? 64 : 128;
    for (int j = 0; j < 8; ++j) {
        tail[tail_len - 1 - j] = static_cast<std::uint8_t>((bit_len >> (8 * j)) & 0xFF);
    }
    for (std::size_t off = 0; off < tail_len; off += 64) { process_block(tail + off); }

    const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int j = 0; j < 5; ++j) {
        digest[static_cast<std::size_t>(j * 4)] = static_cast<std::uint8_t>(hs[j] >> 24);
        digest[static_cast<std::size_t>(j * 4 + 1)] = static_cast<std::uint8_t>(hs[j] >> 16);
        digest[static_cast<std::size_t>(j * 4 + 2)] = static_cast<std::uint8_t>(hs[j] >> 8);
        digest[static_cast<std::size_t>(j * 4 + 3)] = static_cast<std::uint8_t>(hs[j]);
    }
    return digest;
}

std::string sha1_hex(std::string_view s) {
    const auto d = sha1(s.data(), s.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (const std::uint8_t b : d) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

}  // namespace mfweb::util
