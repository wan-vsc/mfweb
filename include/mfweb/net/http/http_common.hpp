#pragma once

// mfweb::http —— HTTP/1.1 公共类型与工具。

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace mfweb::http {

inline constexpr std::string_view kServerName = "mfweb/0.1";

enum class method : std::uint8_t { get, head, post, put, delete_, options, unknown };

[[nodiscard]] method parse_method(std::string_view m) noexcept;
[[nodiscard]] std::string_view method_name(method m) noexcept;

// 状态码 → 原因短语（RFC 7231）
[[nodiscard]] std::string_view status_text(int code) noexcept;

struct header_view {
    std::string_view name;
    std::string_view value;
};

struct request {
    method method_ = method::unknown;
    std::string_view target{};    // 指向解析缓冲区，连接继续读之前有效
    int minor_version = 1;        // HTTP/1.x 里的 x
    std::vector<header_view> headers{};
    std::string_view body{};      // 指向解析缓冲区
    bool keep_alive = true;
    bool chunked = false;

    // 大小写不敏感的头查找；不存在返回空 string_view
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    void reset() noexcept;
};

// RFC 1123 日期（GMT），用于 Date/Last-Modified 头
[[nodiscard]] std::string format_http_date(std::time_t t);
[[nodiscard]] std::string now_http_date();

}  // namespace mfweb::http
