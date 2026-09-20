#pragma once

// mfweb::net::http_client —— 异步 HTTP 客户端（协程风格 + then 风格共享同一内核）。
//
// 两种面貌：
//     coro::task<...> get(...)          → co_await
//     async_result<...> get_async(...)  → .then(...).on_error(...)
// 两者最终都走同一个 request_coro()，不存在两套实现。
//
// 当前实现范围（P8）：
//   * 每次请求新建一条连接（Connection: close）。连接池复用列入 P9 压测调优。
//   * 支持 Content-Length 与 chunked 两种响应体。
//   * 不做重定向跟随、不做 HTTPS（范围明确不含 TLS）。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/json/codec.hpp>
#include <mfweb/net/client/async_result.hpp>
#include <mfweb/net/http/http_common.hpp>
#include <mfweb/net/http/response.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/util/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace mfweb::net {

class http_client {
public:
    using response_type = http::response;
    using outcome = result<response_type, std::error_code>;

    http_client(runtime::io_context& ctx, std::string host, std::uint16_t port)
        : ctx_(&ctx), host_(std::move(host)), port_(port) {}

    // ---- 协程风格
    [[nodiscard]] coro::task<outcome> get(std::string target);
    [[nodiscard]] coro::task<outcome> request(http::method m, std::string target,
                                              std::string body = {});

    // ---- then 风格
    [[nodiscard]] async_result<response_type> get_async(std::string target);
    [[nodiscard]] async_result<response_type> request_async(http::method m, std::string target,
                                                            std::string body = {});

    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    runtime::io_context* ctx_;
    std::string host_;
    std::uint16_t port_;
};

}  // namespace mfweb::net
