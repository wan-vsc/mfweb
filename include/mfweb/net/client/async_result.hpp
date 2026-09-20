#pragma once

// mfweb::net::async_result<T> —— 带 then 链式调用的异步结果。
//
// 动机（简历第 5 条）：协程很好，但**非协程框架**（Qt 事件循环、传统回调式 GUI）
// 没法 co_await。所以同一套异步内核要能同时以两种面貌出现：
//     * 协程风格：  auto r = co_await client.get(url);
//     * then 风格： client.get_async(url).then(...).then(...).on_error(...);
//
// 设计：
//   * 共享状态（shared_ptr）+ 一次性完成（set_value/set_error 只能生效一次）；
//   * 完成时**同步**触发已注册的 continuation —— 调用方在事件循环线程上，
//     因此续接天然回到同一个循环，不需要额外的调度；
//   * then 返回新的 async_result，错误会沿链传播（短路后续 then，直达 on_error）。
//
// 不做的事：不是通用 future（没有超时、没有 cancel、没有多等待者唤醒）。
// 这些留给 io_context 层用更合适的原语处理。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace mfweb::net {

template <class T>
class async_result {
public:
    using value_type = T;

    async_result() : state_(std::make_shared<state>()) {}

    [[nodiscard]] bool ready() const noexcept { return state_->done; }
    [[nodiscard]] bool ok() const noexcept { return state_->done && !state_->err; }
    [[nodiscard]] const std::error_code& error() const noexcept { return state_->err; }

    [[nodiscard]] T& value() { return *state_->value; }
    [[nodiscard]] const T& value() const { return *state_->value; }
    [[nodiscard]] T& operator*() { return value(); }
    [[nodiscard]] T* operator->() { return &value(); }

    void set_value(T v) {
        if (state_->done) { return; }  // 一次性
        state_->value.emplace(std::move(v));
        state_->done = true;
        fire();
    }

    void set_error(std::error_code e) {
        if (state_->done) { return; }
        state_->err = e;
        state_->done = true;
        fire();
    }

    // 续接：fn 收到 T&，返回 U；返回新的 async_result<U>
    template <class F>
    auto then(F&& fn) -> async_result<std::remove_cvref_t<std::invoke_result_t<F, T&>>> {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
        auto next = async_result<U>{};
        auto upstream = *this;
        auto f = std::forward<F>(fn);
        add_continuation([upstream, next, f]() mutable {
            if (upstream.error()) {
                next.set_error(upstream.error());  // 错误沿链传播
                return;
            }
            next.set_value(f(upstream.value()));
        });
        return next;
    }

    // 错误处理：fn 收到 error_code（成功时不调用）
    template <class F>
    async_result& on_error(F&& fn) {
        auto f = std::forward<F>(fn);
        add_continuation([this_state = state_, f]() mutable {
            if (this_state->err) { f(this_state->err); }
        });
        return *this;
    }

private:
    struct state {
        std::optional<T> value;
        std::error_code err{};
        bool done = false;
        std::vector<std::function<void()>> continuations;
    };

    void add_continuation(std::function<void()> fn) {
        if (state_->done) {
            fn();  // 已完成：立即执行
            return;
        }
        state_->continuations.push_back(std::move(fn));
    }

    void fire() {
        auto pending = std::move(state_->continuations);
        state_->continuations.clear();
        for (auto& fn : pending) {
            if (fn) { fn(); }
        }
    }

    std::shared_ptr<state> state_;
};

// void 特化：只关心"完成/失败"
template <>
class async_result<void> {
public:
    using value_type = void;

    async_result() : state_(std::make_shared<state>()) {}

    [[nodiscard]] bool ready() const noexcept { return state_->done; }
    [[nodiscard]] bool ok() const noexcept { return state_->done && !state_->err; }
    [[nodiscard]] const std::error_code& error() const noexcept { return state_->err; }

    void set_value() {
        if (state_->done) { return; }
        state_->done = true;
        fire();
    }

    void set_error(std::error_code e) {
        if (state_->done) { return; }
        state_->err = e;
        state_->done = true;
        fire();
    }

    template <class F>
    auto then(F&& fn) -> async_result<std::remove_cvref_t<std::invoke_result_t<F>>> {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        auto next = async_result<U>{};
        auto upstream = *this;
        auto f = std::forward<F>(fn);
        add_continuation([upstream, next, f]() mutable {
            if (upstream.error()) {
                next.set_error(upstream.error());
                return;
            }
            next.set_value(f());
        });
        return next;
    }

    template <class F>
    async_result& on_error(F&& fn) {
        auto f = std::forward<F>(fn);
        add_continuation([s = state_, f]() mutable {
            if (s->err) { f(s->err); }
        });
        return *this;
    }

private:
    struct state {
        std::error_code err{};
        bool done = false;
        std::vector<std::function<void()>> continuations;
    };

    void add_continuation(std::function<void()> fn) {
        if (state_->done) {
            fn();
            return;
        }
        state_->continuations.push_back(std::move(fn));
    }

    void fire() {
        auto pending = std::move(state_->continuations);
        state_->continuations.clear();
        for (auto& fn : pending) {
            if (fn) { fn(); }
        }
    }

    std::shared_ptr<state> state_;
};

}  // namespace mfweb::net
