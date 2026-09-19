#pragma once

// mfweb::net::connection —— 一条 TCP 连接。
//
// 生命周期是本文件最需要讲清楚的部分，因为它正是"连接超时导致的段错误"的现场：
//
//   1. **谁持有连接**：读循环协程通过 shared_ptr 持有；写协程同理。
//      连接对象只在"没有任何挂起操作"时才会被销毁。
//   2. **关闭姿势**：close() 不销毁任何东西，它只做两件事 ——
//      摘掉空闲超时定时器、取消挂起的 I/O。挂起的读会带着 ERROR_OPERATION_ABORTED
//      被唤醒，读循环因此自然结束并收尾（关套接字、回调 on_close）。
//      **绝不在有挂起 I/O 时销毁协程帧**（ADR-010）。
//   3. **超时定时器**：句柄缓存在成员里，析构函数里取消。
//      这就是简历第 3 条"RAII 析构自动取消定时器"的实际用处 ——
//      如果少了这一步，超时回调会在连接已销毁之后被触发，去访问已释放对象。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/buffer.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mfweb::net {

class connection : public std::enable_shared_from_this<connection> {
public:
    using message_handler = std::function<void(connection&, const char*, std::size_t)>;
    using close_handler = std::function<void(connection&)>;

    connection(runtime::io_context& ctx, io::native_socket socket) noexcept;
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    // 启动读循环。必须在 shared_ptr 上调用（内部会用 shared_from_this）。
    void start();

    // 幂等。摘定时器 + 取消挂起 I/O，不销毁对象。
    void close();

    void send(std::string_view data);
    void send(const void* data, std::size_t len);

    void set_message_handler(message_handler h) { on_message_ = std::move(h); }
    void set_close_handler(close_handler h) { on_close_ = std::move(h); }

    void set_idle_timeout(std::chrono::milliseconds d) noexcept { idle_timeout_ = d; }
    [[nodiscard]] std::chrono::milliseconds idle_timeout() const noexcept { return idle_timeout_; }

    [[nodiscard]] io::native_socket socket() const noexcept { return socket_; }
    [[nodiscard]] bool closing() const noexcept { return closing_; }
    [[nodiscard]] std::size_t received_bytes() const noexcept { return received_; }
    [[nodiscard]] std::size_t sent_bytes() const noexcept { return sent_; }
    [[nodiscard]] bool idle_timer_armed() const noexcept { return idle_timer_.valid(); }

private:
    coro::task<void> read_loop(std::shared_ptr<connection> self);
    coro::task<void> flush_writes(std::shared_ptr<connection> self);

    void arm_idle_timer();
    void disarm_idle_timer() noexcept;

    runtime::io_context* ctx_;
    io::native_socket socket_;

    buffer read_buffer_{8192};
    std::deque<std::string> write_queue_;

    message_handler on_message_;
    close_handler on_close_;

    std::chrono::milliseconds idle_timeout_{30000};

    // 超时定时器句柄。析构里取消 —— 见文件头第 3 条。
    runtime::timer_queue::handle idle_timer_{};

    std::size_t received_ = 0;
    std::size_t sent_ = 0;
    bool started_ = false;
    bool closing_ = false;
    bool write_in_flight_ = false;
};

}  // namespace mfweb::net
