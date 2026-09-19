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

}  // namespace

bool io_context::run_once(bool block, unsigned timeout_ms) {
    unsigned wait_ms = 0;
    if (block) {
        wait_ms = timeout_ms;
        const auto next = loop_.timers().next_deadline();
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
    const bool alive = loop_.run_once(false);
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

void io_context::run() {
    while (!stopped() && has_work()) {
        if (!run_once(true)) { break; }
    }
}

void io_context::spawn(coro::task<void> t) { spawn_body(std::move(t)); }

}  // namespace mfweb::runtime
