#pragma once

// mfweb::net::websocket::ws_session —— 服务端 WebSocket 会话。
//
// 生命周期设计（与 connection 同一套原则）：
//   * 帧读取循环在 run_ws_session 协程里；
//   * 发送走独立 writer 协程 + 共享队列：on_message 是同步回调，不能 co_await，
//     把"发帧"变成"入队 + 唤醒 writer"；
//   * **socket 恰好关闭一次**：读循环退出时若 writer 仍在跑，就把 state.closing 置位，
//     writer 排空队列后负责关 socket；若 writer 没在跑，读循环直接关。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/websocket/ws.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mfweb::net::websocket {

class ws_session {
public:
    struct state_t {
        std::deque<std::string> out_queue;
        bool closing = false;
        bool writer_active = false;
    };

    using message_handler = std::function<void(ws_session&, opcode, std::string_view)>;

    ws_session(runtime::io_context& ctx, io::native_socket socket) noexcept
        : ctx_(&ctx), socket_(socket) {}

    void send(opcode op, std::string_view payload);
    void send_text(std::string_view s) { send(opcode::text, s); }
    void send_binary(std::string_view s) { send(opcode::binary, s); }
    void send_pong(std::string_view payload) { send(opcode::pong, payload); }
    void close(close_code code = close_code::normal);

    message_handler on_message;

private:
    friend coro::task<void> run_ws_session(runtime::io_context& ctx, io::native_socket s,
                                           std::function<void(ws_session&)> handler);

    runtime::io_context* ctx_;
    io::native_socket socket_;
    std::shared_ptr<state_t> state_ = std::make_shared<state_t>();
};

using ws_handler = std::function<void(ws_session&)>;

// 握手已完成后调用：驱动帧读取循环直到连接结束
coro::task<void> run_ws_session(runtime::io_context& ctx, io::native_socket s,
                                ws_handler handler);

}  // namespace mfweb::net::websocket
