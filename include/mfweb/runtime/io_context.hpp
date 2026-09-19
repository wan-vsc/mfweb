#pragma once

// mfweb::runtime::io_context —— 事件循环 + 异步 I/O 引擎的组合体。
//
// 一轮（run_once）做三件事，顺序固定：
//   1. 收割 I/O 完成事件（可能阻塞等待）；
//   2. 执行投递的任务队列（非阻塞）；
//   3. 触发到期的定时器。
// 顺序不能颠倒：完成事件会唤醒协程，而协程往往在恢复后就投递新任务或续上新的 I/O，
// 先把完成事件处理掉能让这些后续动作在同一轮里继续推进。
//
// 工作量计数（work_）决定 run() 何时退出：只要还有挂起的 I/O 操作、投递的任务或
// 未到期的定时器，就继续跑；否则返回。

#include <mfweb/coro/task.hpp>
#include <mfweb/io/iocp_engine.hpp>
#include <mfweb/runtime/event_loop.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <utility>

namespace mfweb::runtime {

class io_context {
public:
    io_context() noexcept = default;
    ~io_context() = default;

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    [[nodiscard]] bool valid() const noexcept { return engine_.valid(); }
    [[nodiscard]] int last_error() const noexcept { return engine_.last_error(); }

    [[nodiscard]] io::iocp_engine& engine() noexcept { return engine_; }
    [[nodiscard]] event_loop& loop() noexcept { return loop_; }
    [[nodiscard]] timer_queue& timers() noexcept { return loop_.timers(); }

    void post(std::function<void()> fn) { loop_.post(std::move(fn)); }

    void stop() {
        loop_.stop();
        engine_.wakeup();  // 把可能阻塞在 harvest 的本线程叫醒
    }

    [[nodiscard]] bool stopped() const noexcept { return loop_.stopped(); }

    // ---- 工作量计数（由 I/O Awaiter 维护）
    void add_work() noexcept { work_.fetch_add(1, std::memory_order_relaxed); }
    void release_work() noexcept { work_.fetch_sub(1, std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pending_io() const noexcept {
        return work_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool has_work() const noexcept {
        return pending_io() > 0 || loop_.has_pending_tasks() || !loop_.timers().empty();
    }

    // 跑一轮。block = true 时最多等待 timeout_ms 毫秒（若有更近的定时器则等更短）。
    bool run_once(bool block, unsigned timeout_ms = 8);

    std::size_t run_until_idle(std::size_t max_rounds = 1000000, unsigned timeout_ms = 8);

    void run();

    // 启动一个顶层协程；协程完成后其帧自动销毁（detached 语义）
    void spawn(coro::task<void> t);

private:
    io::iocp_engine engine_;
    event_loop loop_;
    std::atomic<std::size_t> work_{0};
};

}  // namespace mfweb::runtime
