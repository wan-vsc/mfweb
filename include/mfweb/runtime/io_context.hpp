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
// ---------------------------------------------------------------- 多线程模型
//
// **一个共享 IOCP + N 个每线程事件循环**：
//   * 完成端口（iocp_engine）是所有线程共享的 —— 这正是 IOCP 的设计意图：
//     多个线程同时 GetQueuedCompletionStatus，**内核负责把完成包分给空闲线程**，
//     我们不需要自己做 work-stealing。
//   * 每个线程有独立的 event_loop（任务队列 + 定时器），因此投递任务与定时器操作
//     完全无锁；跨线程投递走 event_loop::post（内部有锁）。
//   * 协程**不绑定线程**：它在哪个线程被唤醒就在哪个线程继续跑。
//     因此协程内的共享状态必须自己保证线程安全（框架的 work_ 计数是原子的）。
//
// 实测收益见 bench/results/http-qps.md：单线程时服务端把一个核跑满（91%）而 QPS 只有
// 2 万；瓶颈就是这里 —— 其余 23 个核在闲着。

#include <mfweb/coro/task.hpp>
#include <mfweb/io/iocp_engine.hpp>
#include <mfweb/runtime/event_loop.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace mfweb::runtime {

class io_context {
public:
    // thread_count = 1 时行为与单线程版本完全一致（测试仍可用 run_once 手动驱动）
    explicit io_context(std::size_t thread_count = 1);
    ~io_context();

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    [[nodiscard]] bool valid() const noexcept { return engine_.valid(); }
    [[nodiscard]] int last_error() const noexcept { return engine_.last_error(); }

    [[nodiscard]] io::iocp_engine& engine() noexcept { return engine_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return loops_.size(); }

    // 指定编号的循环（主要给测试与单线程驱动用）
    [[nodiscard]] event_loop& loop(std::size_t index = 0) noexcept { return *loops_[index]; }
    [[nodiscard]] const event_loop& loop(std::size_t index = 0) const noexcept {
        return *loops_[index];
    }

    // 定时器属于"当前线程的循环"；非工作线程调用时退化到 0 号循环
    [[nodiscard]] timer_queue& timers() noexcept;

    void post(std::function<void()> fn);

    void stop();

    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_relaxed); }

    // ---- 工作量计数（由 I/O Awaiter 维护）
    void add_work() noexcept { work_.fetch_add(1, std::memory_order_relaxed); }
    void release_work() noexcept { work_.fetch_sub(1, std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pending_io() const noexcept {
        return work_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool has_work() const noexcept {
        return pending_io() > 0 || loop(0).has_pending_tasks() || !loop(0).timers().empty();
    }

    // 在当前线程跑一轮（单线程驱动；多线程模式下由 worker 内部调用）
    bool run_once(bool block, unsigned timeout_ms = 8);

    std::size_t run_until_idle(std::size_t max_rounds = 1000000, unsigned timeout_ms = 8);

    // 启动 thread_count 个线程（含调用线程），阻塞直到 stop()
    void run();

    // 启动一个顶层协程；协程完成后其帧自动销毁（detached 语义）
    void spawn(coro::task<void> t);

private:
    void worker(std::size_t index);

    io::iocp_engine engine_;
    std::vector<std::unique_ptr<event_loop>> loops_;
    std::vector<std::thread> threads_;
    std::atomic<std::size_t> work_{0};
    std::atomic<bool> stopped_{false};
};

// 当前线程所属的 io_context（工作线程内有效；非工作线程为 nullptr）
[[nodiscard]] io_context* current_io_context() noexcept;

}  // namespace mfweb::runtime
