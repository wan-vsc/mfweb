#pragma once

// mfweb::util::sha1 —— 自研 SHA-1。
//
// 为什么不引 OpenSSL：WebSocket 握手只需要 SHA-1 这一个原语（RFC 6455 规定
// Sec-WebSocket-Accept = base64(SHA1(key + magic))），为此引入一个加密库违背
// "运行时零第三方依赖"的红线。SHA-1 在 WebSocket 里只做"握手标签"，不承担安全职责
// （安全由 TLS 层负责，本项目不含 TLS，见范围说明）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mfweb::util {

[[nodiscard]] std::array<std::uint8_t, 20> sha1(const void* data, std::size_t len) noexcept;

[[nodiscard]] std::string sha1_hex(std::string_view s);

}  // namespace mfweb::util
