#pragma once

// mfweb::io::uring_engine —— Linux io_uring 后端。
//
// 与 iocp_engine **公开成员完全一致**（见 io/native_engine.hpp 的清单），
// 这样 io_context / socket / server 不需要任何 #ifdef。
//
// 模型仍然是 proactor：调用方提交"把 n 字节读进这块缓冲区"，
// 完成后由 harvest() 唤醒等待它的协程。io_operation 的布局约定同样成立 ——
// 我们把 io_operation* 直接塞进 SQE 的 user_data，收割时原样取回。
//
// 与 Windows 侧的两处**语义差异**（已在实现里注明处理方式）：
//   1. AcceptEx 接受一个"预创建的套接字"，而 io_uring 的 ACCEPT 返回一个**新 fd**。
//      这里用 dup2(new_fd, accepted) 把新连接落到调用方给的 fd 上，保持接口不变。
//   2. IOCP 的 CancelIoEx 让挂起操作以 ERROR_OPERATION_ABORTED 结束；
//      io_uring 用 ASYNC_CANCEL，目标操作以 -ECANCELED 结束。

#include <mfweb/io/io_engine.hpp>

#include <atomic>
#include <cstddef>

#ifndef _WIN32
#include <sys/socket.h>  // AF_INET / sockaddr（make_socket 与 post_connect 的默认参数与形参）
#endif

namespace mfweb::io {

class uring_engine {
public:
    uring_engine() noexcept;
    ~uring_engine();

    uring_engine(const uring_engine&) = delete;
    uring_engine& operator=(const uring_engine&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int last_error() const noexcept {
        return last_error_.load(std::memory_order_relaxed);
    }

    // Linux 上直接 socket(2)；保留此静态函数是为了与 IOCP 侧接口一致
    [[nodiscard]] static native_socket make_socket(int af = AF_INET) noexcept;

    // Linux 上无对应概念（io_uring 提交时自带 fd），保留以统一接口
    bool attach(native_socket s) noexcept;

    bool post_accept(io_operation& op, native_socket listener, native_socket accepted) noexcept;
    bool post_connect(io_operation& op, native_socket s, const sockaddr* addr, int addr_len) noexcept;
    bool post_read(io_operation& op, native_socket s, void* data, std::size_t len) noexcept;
    bool post_write(io_operation& op, native_socket s, const void* data, std::size_t len) noexcept;

    bool cancel(io_operation& op) noexcept;
    bool cancel_socket(native_socket s) noexcept;

    std::size_t harvest(bool block, std::size_t max_events, unsigned timeout_ms);

    void wakeup() noexcept;

private:
    struct impl;
    impl* impl_ = nullptr;
    std::atomic<int> last_error_{0};
};

}  // namespace mfweb::io
