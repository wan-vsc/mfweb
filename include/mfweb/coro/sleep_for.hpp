#pragma once

// mfweb::coro::sleep_for —— 把定时器包成可 co_await 的 Awaiter，并在析构时自动取消。
//
// 这是简历第 3 条"将红黑树迭代器缓存至 Awaiter 对象，结合 RAII 在析构时自动取消定时器"
// 的落点。三条不变量：
//
//   1. **句柄缓存**：await_suspend 里拿到 timer_queue::handle（即红黑树节点指针）并存在
//      Awaiter 里。取消时不需要按键查找，直接 erase 节点。
//   2. **析构即取消**：协程在等待期间被销毁（连接断开、任务被取消）时，Awaiter 析构会
//      把定时器从队列里摘掉 —— 否则定时器到期后会去 resume 一个已经被销毁的协程帧，
//      这正是"连接超时导致的段错误"的经典成因。
//   3. **单次生效**：无论是"定时器先触发"还是"先被取消"，另一条路径都变成空操作：
//        * 定时器触发时，回调第一件事就是 reset 句柄（节点已被队列摘除）；
//        * cancel 成功时句柄也被置空。
//      因此不存在"双完成"，也不存在对已释放节点的再次访问。
//
// 移动语义：Awaiter 必须可移动（co_await 表达式里是临时对象，但容器/可选存储时会移动）。
// 移动后源对象的 queue_ 置空、句柄置空，析构成为空操作。

#include <mfweb/runtime/timer_queue.hpp>

#include <coroutine>
#include <utility>

namespace mfweb::coro {

class timer_awaiter {
public:
    using time_point = runtime::timer_queue::time_point;
    using duration = runtime::timer_queue::duration;

    timer_awaiter(runtime::timer_queue& queue, time_point deadline) noexcept
        : queue_(&queue), deadline_(deadline) {}

    timer_awaiter(timer_awaiter&& other) noexcept
        : queue_(other.queue_),
          deadline_(other.deadline_),
          handle_(other.handle_),
          continuation_(other.continuation_) {
        other.queue_ = nullptr;
        other.handle_.reset();
        other.continuation_ = {};
    }

    timer_awaiter& operator=(timer_awaiter&& other) noexcept {
        if (this != &other) {
            cancel();  // 先释放自己已挂的定时器
            queue_ = other.queue_;
            deadline_ = other.deadline_;
            handle_ = other.handle_;
            continuation_ = other.continuation_;
            other.queue_ = nullptr;
            other.handle_.reset();
            other.continuation_ = {};
        }
        return *this;
    }

    timer_awaiter(const timer_awaiter&) = delete;
    timer_awaiter& operator=(const timer_awaiter&) = delete;

    ~timer_awaiter() { cancel(); }

    // 已经过期就不必挂定时器，直接继续
    [[nodiscard]] bool await_ready() const noexcept {
        return deadline_ <= runtime::timer_queue::clock::now();
    }

    void await_suspend(std::coroutine_handle<> continuation) {
        continuation_ = continuation;
        handle_ = queue_->schedule(deadline_, [this] {
            // 节点已被 fire_expired 摘除并释放，句柄必须立刻失效，
            // 否则析构时会去 cancel 一个已释放的节点。
            handle_.reset();
            continuation_.resume();
        });
    }

    void await_resume() const noexcept {}

    // 主动取消；析构自动调用。可重复调用。
    void cancel() noexcept {
        if (queue_ != nullptr && handle_.valid()) {
            queue_->cancel(handle_);  // cancel 内部会 reset 句柄
        }
    }

    [[nodiscard]] bool pending() const noexcept { return handle_.valid(); }

private:
    runtime::timer_queue* queue_ = nullptr;
    time_point deadline_{};
    runtime::timer_queue::handle handle_{};
    std::coroutine_handle<> continuation_{};
};

[[nodiscard]] inline timer_awaiter sleep_for(runtime::timer_queue& queue,
                                             timer_awaiter::duration delay) {
    return timer_awaiter{queue, runtime::timer_queue::clock::now() + delay};
}

[[nodiscard]] inline timer_awaiter sleep_until(runtime::timer_queue& queue,
                                               timer_awaiter::time_point deadline) {
    return timer_awaiter{queue, deadline};
}

}  // namespace mfweb::coro
