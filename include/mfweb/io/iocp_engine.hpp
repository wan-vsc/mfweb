#pragma once

// mfweb::io::iocp_engine —— Windows IOCP 后端。
//
// 线程模型：**非线程安全**，只应由所属事件循环线程调用。
// 唯一例外是 wakeup()，它用 PostQueuedCompletionStatus 把阻塞在 harvest() 的线程叫醒。
//
// 完成通知为什么用单发 GetQueuedCompletionStatus 而不是 Ex 批量版：
// 批量版（GetQueuedCompletionStatusEx）在条目里**不直接给出每个操作的错误码**，
// 需要额外靠 OVERLAPPED.Internal 或 GetOverlappedResult 还原，容易写错。
// 单发版失败时 GetLastError() 就是该操作的错误码，语义无歧义。
// 批量收割列入 P9 调优项（届时用 GetOverlappedResult 补错误码）。

#include <mfweb/io/io_engine.hpp>

#include <cstddef>

namespace mfweb::io {

class iocp_engine {
public:
    iocp_engine() noexcept;
    ~iocp_engine();

    iocp_engine(const iocp_engine&) = delete;
    iocp_engine& operator=(const iocp_engine&) = delete;

    [[nodiscard]] bool valid() const noexcept { return port_ != nullptr; }
    [[nodiscard]] int last_error() const noexcept { return last_error_; }

    // 创建带 WSA_FLAG_OVERLAPPED 的套接字（IOCP 下必须）
    [[nodiscard]] static native_socket make_socket(int af = AF_INET) noexcept;

    // 把套接字关联到完成端口。所有参与异步 I/O 的套接字都必须先关联。
    bool attach(native_socket s) noexcept;

    // 提交操作。返回 false 表示**提交时就失败**，错误已写入 op.status，
    // 调用方不应等待完成（也不会有完成包）。
    bool post_accept(io_operation& op, native_socket listener, native_socket accepted) noexcept;
    bool post_connect(io_operation& op, native_socket s, const sockaddr* addr, int addr_len) noexcept;
    bool post_read(io_operation& op, native_socket s, void* data, std::size_t len) noexcept;
    bool post_write(io_operation& op, native_socket s, const void* data, std::size_t len) noexcept;

    // 请求取消。取消是异步的：完成包仍会到达，其 error 为 ERROR_OPERATION_ABORTED。
    // 调用方必须仍然等待完成，不能假定操作已经结束。
    bool cancel(io_operation& op) noexcept;

    // 取消该套接字上所有挂起的操作。
    // 用途：连接关闭时把挂起的读唤醒 —— 这是 ADR-010 要求的"先取消、再让协程自己醒来结束"，
    // 而不是直接销毁持有操作对象的协程帧。
    bool cancel_socket(native_socket s) noexcept;

    // 收割完成事件并调用各自的 on_complete。返回本次处理的完成包个数。
    // block = true 时最多等待 timeout_ms 毫秒。
    std::size_t harvest(bool block, std::size_t max_events, unsigned timeout_ms);

    // 线程安全：从任意线程唤醒阻塞中的 harvest
    void wakeup() noexcept;

private:
    bool load_extensions(native_socket probe) noexcept;
    void finish(io_operation* op) noexcept;

    HANDLE port_ = nullptr;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    int last_error_ = 0;
};

}  // namespace mfweb::io
