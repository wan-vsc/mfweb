#pragma once

// mfweb::runtime::event_loop —— 单线程事件循环。
//
// 结构：一个可跨线程投递的任务队列 + 一个定时器队列。
// 循环每轮做两件事：清空任务队列 → 触发到期定时器；空闲时若允许阻塞，
// 就睡到"下一个定时器截止时刻"或"有新任务到达"二者中较早的一个。
//
// 线程模型：
//   * post() 可从任意线程调用（内部有锁）；
//   * 其余操作（timers()、run()）只能在循环线程调用。
//   * P3 会在此挂上 io_engine，把完成事件也并入这一轮的处理。
//
// 为什么"一次把任务队列取空再执行"：回调里常常会再 post 任务，若持锁执行回调会导致
// 自锁，若不取空则可能被持续投递饿死。取快照 + 限定批量是常见折中。

#include <mfweb/runtime/timer_queue.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace mfweb::runtime {

class event_loop {
public:
    using time_point = timer_queue::time_point;

    event_loop() = default;
    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;
    ~event_loop() = default;

    // 线程安全：可从任意线程调用
    void post(std::function<void()> fn);

    // 线程安全。
    //
    // 语义（与 asio::io_context::stop 一致）：
    //   **立即停止，队列中尚未执行的投递任务不会被补跑**，随循环对象析构一起丢弃。
    //   这是有意为之 —— 停止通常发生在关服路径上，此时继续执行排队中的回调
    //   只会延长关闭时间，甚至让已经失效的对象被再次访问。
    //   若需要"把已投递的任务跑完再停"，请先 run_until_idle() 再 stop()。
    void stop() noexcept;

    [[nodiscard]] bool stopped() const noexcept;

    // 仅循环线程
    [[nodiscard]] timer_queue& timers() noexcept { return timers_; }
    [[nodiscard]] const timer_queue& timers() const noexcept { return timers_; }

    // 跑一轮。block = true 且无事可做时阻塞等待。
    // 返回 false 表示循环已停止。
    bool run_once(bool block);

    // 阻塞运行直到 stop()
    void run();

    // 跑到无事可做为止（最多 max_rounds 轮）。测试与单步驱动用。
    std::size_t run_until_idle(std::size_t max_rounds = 1024);

    // 一轮最多执行多少个投递任务（防止回调持续投递导致定时器饿死）
    void set_max_tasks_per_round(std::size_t n) noexcept { max_tasks_per_round_ = n; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    timer_queue timers_;
    bool stopped_ = false;
    std::size_t max_tasks_per_round_ = 256;
};

}  // namespace mfweb::runtime
