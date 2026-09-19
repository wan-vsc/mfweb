#pragma once

// mfweb::coro::generator<T> —— 同步生成器：协程里 co_yield，调用方用范围 for 消费。
//
// 语义：
//   * 惰性：创建时不执行，首次取元素才开始。
//   * 单次遍历（input range）：begin() 只能调用一次，迭代器只有 ++ 没有 --。
//   * 协程内抛出的异常会推迟到迭代到该位置时重新抛出，而不是静默吞掉。
//   * 协程内**不允许 co_await**（生成器不绑定任何调度器），误用会在编译期报错。
//
// 典型用途：流式解析（JSON/HTTP 头）、分块读取、路由匹配的候选枚举。

#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

namespace mfweb::coro {

template <class T>
class [[nodiscard]] generator {
    static_assert(!std::is_reference_v<T>, "generator<T&> 暂不支持");

public:
    struct promise_type {
        std::optional<T> current;
        std::exception_ptr exception;

        generator get_return_object() noexcept {
            return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

        std::suspend_always yield_value(T value) {
            current.emplace(std::move(value));
            return {};
        }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }

        // 生成器不绑定调度器：明确禁止 co_await
        template <class U>
        std::suspend_never await_transform(U&&) = delete;
    };

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        iterator() noexcept = default;
        explicit iterator(std::coroutine_handle<promise_type> handle) : handle_(handle) { advance(); }

        [[nodiscard]] reference operator*() const { return *handle_.promise().current; }
        [[nodiscard]] pointer operator->() const { return std::addressof(*handle_.promise().current); }

        iterator& operator++() {
            advance();
            return *this;
        }

        void operator++(int) { advance(); }

        [[nodiscard]] bool operator==(const iterator& other) const noexcept {
            return handle_ == other.handle_;
        }
        [[nodiscard]] bool operator!=(const iterator& other) const noexcept {
            return handle_ != other.handle_;
        }

    private:
        void advance() {
            if (handle_ == nullptr) { return; }
            if (handle_.done()) {
                handle_ = nullptr;
                return;
            }
            handle_.resume();
            if (handle_.done()) {
                if (handle_.promise().exception) {
                    std::rethrow_exception(handle_.promise().exception);
                }
                handle_ = nullptr;
            }
        }

        std::coroutine_handle<promise_type> handle_{};
    };

    using handle_type = std::coroutine_handle<promise_type>;

    generator() noexcept = default;

    generator(generator&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    generator& operator=(generator&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    ~generator() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    [[nodiscard]] iterator begin() { return iterator{handle_}; }
    [[nodiscard]] iterator end() noexcept { return iterator{}; }

private:
    explicit generator(handle_type handle) noexcept : handle_(handle) {}

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_{};
};

}  // namespace mfweb::coro
