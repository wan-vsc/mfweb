#pragma once

// mfweb::util::base64 —— 自研 Base64 编解码（RFC 4648 标准字母表）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace mfweb::util {

[[nodiscard]] std::string base64_encode(const void* data, std::size_t len);

[[nodiscard]] inline std::string base64_encode(std::string_view s) {
    return base64_encode(s.data(), s.size());
}

// 解码；返回 false 表示非法输入（长度或字符非法）
[[nodiscard]] bool base64_decode(std::string_view s, std::vector<std::uint8_t>& out);

}  // namespace mfweb::util
