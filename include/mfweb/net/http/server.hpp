#pragma once

// mfweb::http::server —— 极简 HTTP/1.1 服务器。
//
// 模型：每个连接一个协程（read → parse → respond → keep-alive 循环）。
// 用起来无需线程池：io_context 已经是事件循环，协程天然并发。
//
// 当前能力（P4）：
//   * 静态文件服务（含 Range 断点续传、ETag/Last-Modified 条件请求）；
//   * 404 兜底。
// 路由、AOP、WebSocket、POST 动态 handler 在 P6/P5 加入。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/http/http_common.hpp>
#include <mfweb/net/http/parser.hpp>
#include <mfweb/net/http/response.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace mfweb::http {

class server {
public:
    explicit server(runtime::io_context& ctx) noexcept : ctx_(&ctx) {}

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // 监听并启动接受循环。返回 false 表示监听失败。
    bool listen(std::uint16_t port, const char* bind_ip = "127.0.0.1");

    // 把 URL 前缀映射到磁盘目录（例如 "/files" → "./www"）
    void serve_static(std::string url_prefix, std::string root);

    // 驱动事件循环直到 stop()
    void run();

    void stop() { ctx_->stop(); }

    [[nodiscard]] io::native_socket listener() const noexcept { return listener_; }

private:
    coro::task<void> accept_loop(io::native_socket listener, std::string prefix, std::string root);
    coro::task<void> serve_connection(io::native_socket s, std::string prefix, std::string root);
    coro::task<bool> write_response(io::native_socket s, const response& resp, bool head_only);
    [[nodiscard]] response handle_request(const request& req, const std::string& prefix,
                                          const std::string& root) const;

    runtime::io_context* ctx_;
    io::native_socket listener_ = io::k_invalid_socket;
    std::string static_prefix_;
    std::string static_root_;
};

}  // namespace mfweb::http
