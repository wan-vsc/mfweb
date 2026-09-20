#include <mfweb/runtime/io_context.hpp>

#include <algorithm>
#include <chrono>

namespace mfweb::runtime {
namespace {

struct spawn_root {
    struct promise_type {
        spawn_root get_return_object() noexcept { return {}; }
        [[nodiscard]] std::suspend_never initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
};

spawn_root spawn_body(coro::task<void> t) { co_await std::move(t); }

// 每个工作线程记住自己是哪个 io_context 的哪个循环
thread_local io_context* tls_ctx = nullptr;
thread_local std::size_t tls_loop_index = 0;

// 工作量计数分片：每个线程固定用一个分片，避免所有线程抢同一条缓存行
thread_local std::size_t tls_shard = 0;
std::atomic<std::size_t> g_next_shard{1};  // 0 号留给非工作线程

constexpr unsigned k_max_wait_ms = 8;

}  // namespace

io_context* current_io_context() noexcept { return tls_ctx; }

io_context::io_context(std::size_t thread_count) {
    const std::size_t n = (thread_count == 0) ? 1 : thread_count;
    loops_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { loops_.push_back(std::make_unique<event_loop>()); }
}

io_context::~io_context() {
    stop();
    for (auto& t : threads_) {
        if (t.joinable()) { t.join(); }
    }
}

timer_queue& io_context::timers() noexcept {
    if (tls_ctx == this && tls_loop_index < loops_.size()) {
        return loops_[tls_loop_index]->timers();
    }
    return loops_[0]->timers();
}

void io_context::post(std::function<void()> fn) {
    if (tls_ctx == this && tls_loop_index < loops_.size()) {
        loops_[tls_loop_index]->post(std::move(fn));  // 同线程：进本线程队列，无需跨线程唤醒
    } else {
        loops_[0]->post(std::move(fn));
    }
}

void io_context::stop() {
    stopped_.store(true, std::memory_order_relaxed);
    for (auto& l : loops_) { l->stop(); }
    // 每个可能阻塞在 GQCS 的线程都要一个唤醒包（PostQueuedCompletionStatus 只唤醒一个）
    for (std::size_t i = 0; i < loops_.size(); ++i) { engine_.wakeup(); }
}

bool io_context::run_once(bool block, unsigned timeout_ms) {
    event_loop& lp = *loops_[0];
    unsigned wait_ms = 0;
    if (block) {
        wait_ms = timeout_ms;
        const auto next = lp.timers().next_deadline();
        if (next != timer_queue::time_point::max()) {
            const auto now = timer_queue::clock::now();
            if (next <= now) {
                wait_ms = 0;
            } else {
                const auto delta =
                    std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
                wait_ms = static_cast<unsigned>(
                    std::min<long long>(delta < 0 ? 0 : delta, static_cast<long long>(timeout_ms)));
            }
        }
    }

    engine_.harvest(block && wait_ms > 0, 512, wait_ms);
    // 完成事件唤醒了协程，协程可能又投递了任务或挂上了新 I/O，这里非阻塞地把它们推进一轮
    const bool alive = lp.run_once(false);
    return alive;
}

std::size_t io_context::run_until_idle(std::size_t max_rounds, unsigned timeout_ms) {
    std::size_t rounds = 0;
    while (rounds < max_rounds && !stopped() && has_work()) {
        if (!run_once(true, timeout_ms)) { break; }
        ++rounds;
    }
    return rounds;
}

// 每个线程固定用一个分片，避免所有线程抢同一条缓存行
[[nodiscard]] std::size_t shard_of_current_thread() noexcept {
    if (tls_shard == 0) { tls_shard = g_next_shard.fetch_add(1) % io_context::k_work_shards; }
    return tls_shard;
}

void io_context::add_work() noexcept {
    work_shards_[shard_of_current_thread()].count.fetch_add(1, std::memory_order_relaxed);
}

void io_context::release_work() noexcept {
    work_shards_[shard_of_current_thread()].count.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t io_context::pending_io() const noexcept {
    std::int64_t total = 0;
    for (const auto& shard : work_shards_) {
        total += shard.count.load(std::memory_order_relaxed);
    }
    return total > 0 ? static_cast<std::size_t>(total) : 0;
}

void io_context::worker(std::size_t index) {
    tls_ctx = this;
    tls_loop_index = index;
    tls_shard = (index + 1) % k_work_shards;  // 每个 worker 固定一个分片
    event_loop& lp = *loops_[index];

    while (!stopped_.load(std::memory_order_relaxed)) {
        // 等多久：不超过固定上限，也不超过本线程最近一个定时器的到期时间
        unsigned wait = k_max_wait_ms;
        const auto next = lp.timers().next_deadline();
        if (next != timer_queue::time_point::max()) {
            const auto now = timer_queue::clock::now();
            if (next <= now) {
                wait = 0;
            } else {
                const auto delta =
                    std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
                wait = static_cast<unsigned>(
                    std::min<long long>(delta < 0 ? 0 : delta, k_max_wait_ms));
            }
        }

        // 共享完成端口：内核把完成包分给最先来取的线程
        engine_.harvest(wait > 0, 256, wait);
        lp.run_once(false);
    }

    tls_ctx = nullptr;
}

void io_context::run() {
    const std::size_t n = loops_.size();
    if (n > 1) {
        threads_.reserve(n - 1);
        for (std::size_t i = 1; i < n; ++i) {
            threads_.emplace_back([this, i] { worker(i); });
        }
    }
    worker(0);  // 调用线程也参与，不额外多起一个
    for (auto& t : threads_) {
        if (t.joinable()) { t.join(); }
    }
    threads_.clear();
}

void io_context::spawn(coro::task<void> t) { spawn_body(std::move(t)); }

}  // namespace mfweb::runtime
