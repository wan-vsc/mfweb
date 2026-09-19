#pragma once

// mfweb::http —— Range（断点续传）解析与实体校验。
//
// 实现 RFC 7233 的字节区间语法：
//   bytes=a-b      闭区间
//   bytes=a-       从 a 到结尾
//   bytes=-N       最后 N 字节
//   bytes=a-b,c-d  多区间
// 任何一个区间不满足（a > b、a 越界、N == 0）→ 整个请求判 416。

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace mfweb::http {

struct byte_range {
    std::uint64_t first = 0;  // 闭区间
    std::uint64_t last = 0;

    [[nodiscard]] std::uint64_t length() const noexcept { return last - first + 1; }
};

// 解析 Range 头。
//   header 为空或不是 bytes= 形式 → ok=true 且 out 为空（无 Range，应回 200 全量）；
//   解析失败（数字非法等）→ 返回 false（应回 400）；
//   语法合法但区间不满足 → ok=true 且 unsatisfiable=true（应回 416）。
[[nodiscard]] bool parse_range(std::string_view header, std::uint64_t file_size,
                               std::vector<byte_range>& out, bool& unsatisfiable);

// 基于 (size, mtime) 生成强 ETag
[[nodiscard]] std::string make_etag(std::uint64_t size, std::time_t mtime);

// 简单实体标签匹配：支持 "*"（资源存在即命中）与逗号分隔的标签列表。
// header 为空返回 false。
[[nodiscard]] bool etag_matches(std::string_view header, std::string_view etag);

}  // namespace mfweb::http
