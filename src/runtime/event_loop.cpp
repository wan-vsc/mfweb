#include <mfweb/runtime/event_loop.hpp>

#include <utility>

namespace mfweb::runtime {

void event_loop::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void event_loop::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
}

bool event_loop::stopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

bool event_loop::run_once(bool block) {
    std::deque<std::function<void()>> batch;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) { return false; }

        if (block && tasks_.empty()) {
            const time_point next = timers_.next_deadline();
            if (next == time_point::max()) {
                cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
            } else {
                // 睡到最近的定时器截止时刻；若有唤醒则提前返回。
                // 用 wait_until + 谓词：虚假唤醒与提前唤醒都会重新检查条件。
                (void)cv_.wait_until(lock, next,
                                     [this] { return stopped_ || !tasks_.empty(); });
            }
        }

        if (stopped_) { return false; }

        const std::size_t take =
            (tasks_.size() < max_tasks_per_round_) ? tasks_.size() : max_tasks_per_round_;
        for (std::size_t i = 0; i < take; ++i) {
            batch.push_back(std::move(tasks_.front()));
            tasks_.pop_front();
        }
    }

    for (auto& fn : batch) {
        if (fn) { fn(); }
    }

    timers_.fire_expired(timer_queue::clock::now());
    return !stopped();
}

void event_loop::run() {
    while (run_once(true)) {
    }
}

std::size_t event_loop::run_until_idle(std::size_t max_rounds) {
    std::size_t rounds = 0;
    while (rounds < max_rounds) {
        // 非阻塞：没有任务也没有到期定时器时返回 false
        bool did_work = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            did_work = !tasks_.empty();
        }
        if (!did_work) {
            did_work = timers_.next_deadline() <= timer_queue::clock::now();
        }
        if (!did_work) { break; }

        if (!run_once(false)) { break; }
        ++rounds;
    }
    return rounds;
}

}  // namespace mfweb::runtime
