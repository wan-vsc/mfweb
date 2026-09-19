#pragma once

// mfweb::coro —— 把 I/O 引擎的操作包成可 co_await 的对象。
//
// ⚠️ 生命周期约束（本文件最重要的一段）
//
// proactor 模型下，**操作对象必须活到完成包到达为止**。这里的操作对象
// （io::io_operation）就放在 Awaiter 里，而 Awaiter 是 co_await 表达式中的临时对象，
// 其存活期随协程帧。因此有：
//
//     绝不能在"有挂起 I/O 的情况下"销毁协程帧。
//
// 违反它的后果不是立刻崩溃，而是完成包到达时往已释放的帧里写 —— 典型表现是
// 连接超时场景下的 use-after-free（这正是简历第 2 条要复现的那个缺陷）。
//
// 正确用法是**先取消、再让协程自己醒来结束**：连接关闭时对挂起的操作调用 cancel()，
// 协程会带着 ERROR_OPERATION_ABORTED 被唤醒并正常跑完，帧的生命周期由协程自己掌握。
// 析构里的 cancel 只是"操作已完成但对象还没销毁"这一安全情形下的兜底。
//
// 这条约束在 P3 的 connection 层用侵入式引用计数强制保证，并在 docs/bugs/ 里
// 留下 ASAN 复现与修复的完整证据。

#include <mfweb/io/io_engine.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/util/assert.hpp>

#include <coroutine>
#include <cstddef>
#include <utility>

namespace mfweb::coro {

// 完成回调：函数指针而非 std::function —— 它会被写进 io_operation，
// 必须是无状态的（也不能有捕获）。上下文通过 op->user 传回。
inline void resume_on_io_complete(io::io_operation* op) noexcept {
    if (auto* ctx = static_cast<runtime::io_context*>(op->user)) { ctx->release_work(); }
    // 先把句柄取出来并清空：resume 之后 op 所在的对象可能已被销毁，
    // 绝不能在 resume 之后再碰 op。
    const std::coroutine_handle<> h = op->continuation;
    op->continuation = {};
    if (h) { h.resume(); }
}

class io_awaiter {
public:
    io_awaiter(runtime::io_context& ctx, io::native_socket socket) noexcept
        : ctx_(&ctx), socket_(socket) {}

    io_awaiter(io_awaiter&& other) noexcept
        : ctx_(other.ctx_), socket_(other.socket_), op_(other.op_), pending_(other.pending_) {
        other.ctx_ = nullptr;
        other.pending_ = false;
        other.op_.on_complete = nullptr;
        other.op_.continuation = {};
    }

    io_awaiter& operator=(io_awaiter&&) = delete;
    io_awaiter(const io_awaiter&) = delete;
    io_awaiter& operator=(const io_awaiter&) = delete;

    ~io_awaiter() {
        // 兜底：操作已提交但还没收到完成包时，请求取消。
        // 注意取消是异步的 —— 完成包仍会到达，所以这里不能让对象就此消失；
        // 真正的安全保证来自"不在挂起期间销毁协程"这条上层约束。
        if (pending_ && ctx_ != nullptr) { ctx_->engine().cancel(op_); }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    // 返回 false 表示提交时即失败：不挂起，直接继续，错误在 await_resume 里取。
    bool await_suspend(std::coroutine_handle<> continuation) {
        op_.on_complete = &resume_on_io_complete;
        op_.continuation = continuation;
        op_.user = ctx_;

        ctx_->add_work();
        const bool submitted = submit();
        if (!submitted) {
            ctx_->release_work();
            op_.on_complete = nullptr;
            op_.continuation = {};
            return false;  // 不挂起
        }
        pending_ = true;
        return true;
    }

    [[nodiscard]] io::io_status await_resume() noexcept {
        pending_ = false;
        return op_.status;
    }

    // 供上层主动取消（例如连接超时）
    bool cancel() noexcept {
        if (!pending_ || ctx_ == nullptr) { return false; }
        return ctx_->engine().cancel(op_);
    }

    [[nodiscard]] bool pending() const noexcept { return pending_; }

protected:
    virtual bool submit() = 0;

    runtime::io_context* ctx_ = nullptr;
    io::native_socket socket_ = io::k_invalid_socket;
    io::io_operation op_{};
    bool pending_ = false;
};

// ---------------------------------------------------------------- 具体操作

class read_awaiter final : public io_awaiter {
public:
    read_awaiter(runtime::io_context& ctx, io::native_socket s, void* data, std::size_t len) noexcept
        : io_awaiter(ctx, s), data_(data), len_(len) {}

protected:
    bool submit() override { return ctx_->engine().post_read(op_, socket_, data_, len_); }

private:
    void* data_;
    std::size_t len_;
};

class write_awaiter final : public io_awaiter {
public:
    write_awaiter(runtime::io_context& ctx, io::native_socket s, const void* data,
                  std::size_t len) noexcept
        : io_awaiter(ctx, s), data_(data), len_(len) {}

protected:
    bool submit() override { return ctx_->engine().post_write(op_, socket_, data_, len_); }

private:
    const void* data_;
    std::size_t len_;
};

class accept_awaiter final : public io_awaiter {
public:
    accept_awaiter(runtime::io_context& ctx, io::native_socket listener,
                   io::native_socket accepted) noexcept
        : io_awaiter(ctx, listener), accepted_(accepted) {}

protected:
    bool submit() override { return ctx_->engine().post_accept(op_, socket_, accepted_); }

private:
    io::native_socket accepted_;
};

class connect_awaiter final : public io_awaiter {
public:
    connect_awaiter(runtime::io_context& ctx, io::native_socket s, const sockaddr* addr,
                    int addr_len) noexcept
        : io_awaiter(ctx, s), addr_(addr), addr_len_(addr_len) {}

protected:
    bool submit() override { return ctx_->engine().post_connect(op_, socket_, addr_, addr_len_); }

private:
    const sockaddr* addr_;
    int addr_len_;
};

[[nodiscard]] inline read_awaiter async_read(runtime::io_context& ctx, io::native_socket s,
                                             void* data, std::size_t len) noexcept {
    return read_awaiter{ctx, s, data, len};
}

[[nodiscard]] inline write_awaiter async_write(runtime::io_context& ctx, io::native_socket s,
                                               const void* data, std::size_t len) noexcept {
    return write_awaiter{ctx, s, data, len};
}

[[nodiscard]] inline accept_awaiter async_accept(runtime::io_context& ctx,
                                                 io::native_socket listener,
                                                 io::native_socket accepted) noexcept {
    return accept_awaiter{ctx, listener, accepted};
}

[[nodiscard]] inline connect_awaiter async_connect(runtime::io_context& ctx, io::native_socket s,
                                                   const sockaddr* addr, int addr_len) noexcept {
    return connect_awaiter{ctx, s, addr, addr_len};
}

}  // namespace mfweb::coro
