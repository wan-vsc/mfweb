#pragma once

// mfweb::result<T, E> —— 值/错误的二选一结果类型。
//
// 为什么不用 std::expected：它是 C++23 的。本项目语言基线严格锁定 C++20
// （不使用 /std:c++latest），因此自行实现。
//
// 用法：
//     mfweb::result<int, std::error_code> f();
//     if (auto r = f()) { use(r.value()); } else { log(r.error()); }
//     return mfweb::ok(42);
//     return mfweb::err(std::make_error_code(std::errc::invalid_argument));
//
// 约定：
//   * 对处于错误状态的 result 调用 value() 属于违反前置条件，会触发 MFWEB_ASSERT 终止进程，
//     而不是抛异常 —— 框架热路径不依赖异常。
//   * T 与 E 不能是同一种类型（否则构造会产生歧义），编译期即报错。

#include <mfweb/util/assert.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace mfweb {

template <class T>
struct ok_wrapper {
    T value;
};

template <class E>
struct err_wrapper {
    E error;
};

template <class T>
[[nodiscard]] constexpr ok_wrapper<std::decay_t<T>> ok(T&& value) {
    return ok_wrapper<std::decay_t<T>>{std::forward<T>(value)};
}

template <class E>
[[nodiscard]] constexpr err_wrapper<std::decay_t<E>> err(E&& error) {
    return err_wrapper<std::decay_t<E>>{std::forward<E>(error)};
}

template <class T, class E = void>
class [[nodiscard]] result;

// ---------------------------------------------------------------- result<T, E>
template <class T, class E>
class [[nodiscard]] result {
    static_assert(!std::is_same_v<std::decay_t<T>, std::decay_t<E>>,
                  "result<T, E> 的 T 与 E 不能是同一类型");

public:
    using value_type = T;
    using error_type = E;

    template <class U>
    result(ok_wrapper<U>&& w) : has_value_(true) {
        ::new (static_cast<void*>(std::addressof(storage_.value))) T(std::move(w.value));
    }

    template <class U>
    result(err_wrapper<U>&& w) : has_value_(false) {
        ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(w.error));
    }

    result(result&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                    std::is_nothrow_move_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (static_cast<void*>(std::addressof(storage_.value))) T(std::move(other.storage_.value));
        } else {
            ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(other.storage_.error));
        }
    }

    result(const result& other)
        : has_value_(other.has_value_) {
        if (has_value_) {
            ::new (static_cast<void*>(std::addressof(storage_.value))) T(other.storage_.value);
        } else {
            ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
        }
    }

    result& operator=(result&& other) noexcept(std::is_nothrow_move_assignable_v<T> &&
                                               std::is_nothrow_move_assignable_v<E>) {
        if (this != std::addressof(other)) {
            destroy();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(std::addressof(storage_.value))) T(std::move(other.storage_.value));
            } else {
                ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(other.storage_.error));
            }
        }
        return *this;
    }

    result& operator=(const result& other) {
        if (this != std::addressof(other)) {
            destroy();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(std::addressof(storage_.value))) T(other.storage_.value);
            } else {
                ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
            }
        }
        return *this;
    }

    ~result() { destroy(); }

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

    [[nodiscard]] T& value() & {
        MFWEB_ASSERT(has_value_);
        return storage_.value;
    }
    [[nodiscard]] const T& value() const& {
        MFWEB_ASSERT(has_value_);
        return storage_.value;
    }
    [[nodiscard]] T&& value() && {
        MFWEB_ASSERT(has_value_);
        return std::move(storage_.value);
    }

    [[nodiscard]] E& error() & {
        MFWEB_ASSERT(!has_value_);
        return storage_.error;
    }
    [[nodiscard]] const E& error() const& {
        MFWEB_ASSERT(!has_value_);
        return storage_.error;
    }
    [[nodiscard]] E&& error() && {
        MFWEB_ASSERT(!has_value_);
        return std::move(storage_.error);
    }

    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T* operator->() { return std::addressof(value()); }
    [[nodiscard]] const T* operator->() const { return std::addressof(value()); }

    template <class U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return has_value_ ? storage_.value : static_cast<T>(std::forward<U>(fallback));
    }

    template <class U>
    [[nodiscard]] T value_or(U&& fallback) && {
        return has_value_ ? std::move(storage_.value) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    void destroy() noexcept {
        if (has_value_) {
            storage_.value.~T();
        } else {
            storage_.error.~E();
        }
    }

    union storage_t {
        T value;
        E error;
        storage_t() noexcept {}
        ~storage_t() noexcept {}
    };

    storage_t storage_{};
    bool has_value_;
};

// ---------------------------------------------------------------- result<void, E>
template <class E>
class [[nodiscard]] result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    // 默认构造 = 成功（无值可携带）
    result() noexcept = default;

    template <class U>
    result(err_wrapper<U>&& w) : has_value_(false) {
        ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(w.error));
    }

    result(result&& other) noexcept(std::is_nothrow_move_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (!has_value_) {
            ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(other.storage_.error));
        }
    }

    result(const result& other) : has_value_(other.has_value_) {
        if (!has_value_) {
            ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
        }
    }

    result& operator=(result&& other) noexcept(std::is_nothrow_move_assignable_v<E>) {
        if (this != std::addressof(other)) {
            destroy();
            has_value_ = other.has_value_;
            if (!has_value_) {
                ::new (static_cast<void*>(std::addressof(storage_.error))) E(std::move(other.storage_.error));
            }
        }
        return *this;
    }

    result& operator=(const result& other) {
        if (this != std::addressof(other)) {
            destroy();
            has_value_ = other.has_value_;
            if (!has_value_) {
                ::new (static_cast<void*>(std::addressof(storage_.error))) E(other.storage_.error);
            }
        }
        return *this;
    }

    ~result() { destroy(); }

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

    void value() const noexcept { MFWEB_ASSERT(has_value_); }

    [[nodiscard]] E& error() & {
        MFWEB_ASSERT(!has_value_);
        return storage_.error;
    }
    [[nodiscard]] const E& error() const& {
        MFWEB_ASSERT(!has_value_);
        return storage_.error;
    }
    [[nodiscard]] E&& error() && {
        MFWEB_ASSERT(!has_value_);
        return std::move(storage_.error);
    }

private:
    void destroy() noexcept {
        if (!has_value_) { storage_.error.~E(); }
    }

    union storage_t {
        E error;
        storage_t() noexcept {}
        ~storage_t() noexcept {}
    };

    storage_t storage_{};
    bool has_value_ = true;
};

}  // namespace mfweb
