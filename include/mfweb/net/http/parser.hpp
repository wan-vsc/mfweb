#pragma once

// mfweb::http::request_parser —— HTTP/1.1 请求解析器。
//
// 实现策略：
//   * 请求头（请求行 + 各头字段）先累积进内部缓冲，遇到空行（CRLF CRLF）一次性解析；
//   * 请求体按 Content-Length 累积（超出上限即报 413）；
//   * 解析过程是**单趟扫描、非递归**（只有显式的状态转移，没有递归下降）。
//
// 安全上限（默认）：
//   * 头部总长 16 KiB —— 超过返回 error（服务端应回 431）；
//   * 请求体 64 MiB —— 超过返回 error（服务端应回 413）。

#include <mfweb/net/http/http_common.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace mfweb::http {

class request_parser {
public:
    enum class result { need_more, complete, error };

    explicit request_parser(std::size_t max_head = 16 * 1024,
                            std::size_t max_body = 64ull * 1024 * 1024) noexcept;

    // 喂入一段数据。consumed 返回本次实际消费的字节数（可能小于 len，因为请求已经完整）。
    result feed(const char* data, std::size_t len, request& out, std::size_t& consumed);

    void reset() noexcept;

    [[nodiscard]] std::string_view error_message() const noexcept { return error_; }

private:
    // 头部已完整（空行已找到），解析请求行与头字段；返回 false 表示格式错误。
    bool parse_head(request& out);
    static bool parse_header_line(std::string_view line, header_view& out);

    // 请求体的两种编码
    enum class body_mode : std::uint8_t { none, length, chunked };
    // chunked 解码状态机：size → data → data_crlf →（循环）→ trailer
    enum class chunk_state : std::uint8_t { size, data, data_crlf, trailer, done };

    result feed_chunked(request& out);

    std::string head_;       // 请求行 + 头部，到空行为止
    std::string body_;       // 解码后的请求体
    std::string chunk_raw_;  // chunked 原始字节（未解码）
    std::size_t max_head_;
    std::size_t max_body_;
    std::size_t content_length_ = 0;
    std::size_t chunk_remaining_ = 0;
    body_mode mode_ = body_mode::none;
    chunk_state chunk_state_ = chunk_state::size;
    bool head_done_ = false;
    std::string_view error_{"ok"};
};

}  // namespace mfweb::http
