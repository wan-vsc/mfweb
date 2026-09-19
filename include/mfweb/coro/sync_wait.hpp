#pragma once

// sync_wait —— 在当前线程把 task 跑到完成。
//
// 适用范围：任务链不依赖外部事件源（无 I/O、无定时器）时使用；测试与基准大量使用它。
// 若任务在 sync_wait 期间挂起且无人恢复，会触发断言而不是静默死锁 ——
// 这类任务应当交给 io_context 驱动（P2 起）。

#include <mfweb/coro/task.hpp>
#include <mfweb/util/assert.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace mfweb::coro {
namespace detail {

struct sync_wait_root {
    struct promise_type {
        std::exception_ptr exception;

        sync_wait_root get_return_object() noexcept {
            return sync_wait_root{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle{};
};

// 结果槽：T 为 void 时退化为空类型。
// 不能直接用 std::optional<T> —— 即便写在 if constexpr 里，
// 变量声明本身也会被实例化，std::optional<void> 非法。
template <class T>
struct value_slot {
    std::optional<T> storage;

    void set(T value) { storage.emplace(std::move(value)); }

    T take() {
        MFWEB_ASSERT(storage.has_value());
        return std::move(*storage);
    }
};

template <>
struct value_slot<void> {
    void set() noexcept {}
    void take() const noexcept {}
};

template <class T>
sync_wait_root sync_wait_body(task<T> t, value_slot<T>& slot) {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(t);
    } else {
        slot.set(co_await std::move(t));
    }
}

}  // namespace detail

template <class T = void>
T sync_wait(task<T> t) {
    detail::value_slot<T> slot;
    detail::sync_wait_root root = detail::sync_wait_body<T>(std::move(t), slot);

    root.handle.resume();
    MFWEB_ASSERT_MSG(root.handle.done(),
                     "sync_wait 期间任务被挂起且无人恢复：该任务需要由 io_context 驱动");
    std::exception_ptr eptr = root.handle.promise().exception;
    root.handle.destroy();

    if (eptr) { std::rethrow_exception(eptr); }

    if constexpr (!std::is_void_v<T>) {
        return slot.take();
    } else {
        slot.take();
    }
}

}  // namespace mfweb::coro
