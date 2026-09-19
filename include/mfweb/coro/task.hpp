#pragma once

// mfweb::coro::task<T> —— 惰性协程任务。
//
// 关键性质：
//   1. **惰性**：initial_suspend 挂起，只有被 co_await 或 sync_wait 时才真正开始执行。
//   2. **对称转移**：协程结束时把控制权**直接**交给等待者（返回 continuation 句柄），
//      而不是层层 resume 回来。因此 co_await 嵌套深度与栈深度无关 ——
//      测试 test_coro_task.cpp/deep_nesting_* 用 10 万层嵌套验证这一点。
//   3. **帧分配走自研分配器**（frame_allocator），过对齐帧退化为全局堆。
//   4. 异常经 std::exception_ptr 存于 promise，在 await_resume 处重抛。
//
// 限制：
//   * 只支持移动，不支持拷贝。
//   * 同一个 task 只能被 await 一次。

#include <mfweb/coro/frame_allocator.hpp>
#include <mfweb/util/assert.hpp>

#include <coroutine>
#include <exception>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace mfweb::coro {

template <class T>
class [[nodiscard]] task;

namespace detail {

// 所有 task promise 共用的部分：continuation 记录与帧分配
struct task_promise_base {
    std::coroutine_handle<> continuation{};

    static void* operator new(std::size_t size) { return frame_allocator::allocate(size); }

    static void operator delete(void* ptr, std::size_t size) noexcept {
        frame_allocator::deallocate(ptr, size);
    }

    // 过对齐帧不进池，直接走全局堆的 aligned 版本
    static void* operator new(std::size_t size, std::align_val_t align) {
        return ::operator new(size, align);
    }

    static void operator delete(void* ptr, std::size_t /*size*/, std::align_val_t align) noexcept {
        ::operator delete(ptr, align);
    }
};

// 协程到达 final suspend 时对称转移到 continuation
template <class Promise>
struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        std::coroutine_handle<> cont = handle.promise().continuation;
        return cont ? cont : std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

template <class T>
struct task_promise : task_promise_base {
    std::optional<T> value;
    std::exception_ptr exception;

    task<T> get_return_object() noexcept;

    [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
    [[nodiscard]] final_awaiter<task_promise> final_suspend() noexcept { return {}; }

    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    T take_result() {
        if (exception) { std::rethrow_exception(exception); }
        MFWEB_ASSERT(value.has_value());
        return std::move(*value);
    }
};

template <>
struct task_promise<void> : task_promise_base {
    std::exception_ptr exception;

    task<void> get_return_object() noexcept;

    [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
    [[nodiscard]] final_awaiter<task_promise<void>> final_suspend() noexcept { return {}; }

    void return_void() noexcept {}
    void unhandled_exception() noexcept { exception = std::current_exception(); }

    void take_result() {
        if (exception) { std::rethrow_exception(exception); }
    }
};

}  // namespace detail

template <class T = void>
class [[nodiscard]] task {
    static_assert(!std::is_reference_v<T>, "task<T&> 暂不支持；请返回值或指针");
    static_assert(std::is_move_constructible_v<T> || std::is_void_v<T>,
                  "task<T> 要求 T 可移动构造");

public:
    using promise_type = detail::task_promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept = default;

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    ~task() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }

    // ---------------------------------------------------------- awaitable
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        MFWEB_ASSERT(handle_);
        handle_.promise().continuation = continuation;
        return handle_;  // 对称转移：直接开始被等待的协程，不增长调用栈
    }

    T await_resume() { return handle_.promise().take_result(); }

private:
    friend struct detail::task_promise<T>;

    explicit task(handle_type handle) noexcept : handle_(handle) {}

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_{};
};

namespace detail {

template <class T>
task<T> task_promise<T>::get_return_object() noexcept {
    return task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
    return task<void>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

}  // namespace detail

}  // namespace mfweb::coro
