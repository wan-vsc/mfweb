#pragma once

// mfweb::util::small_vector —— 内联容量 + 堆回退的小向量。
//
// 用途：路由匹配这类"每次调用都需要一个短生命周期小容器"的场景。
// 直接用 std::vector 会在每次匹配里做一次 malloc —— 基准实测中这正是
// **非递归匹配比递归下降还慢 14%** 的原因（递归版用调用栈，不分配）。
//
// 设计取舍：
//   * 只支持**可平凡复制**的元素（内部用 memcpy 扩容），够用且实现简单；
//   * 内联容量 N 用满后一次性堆分配，之后按倍增扩容；
//   * 不提供迭代器 —— 只需要 push_back/pop_back/operator[]/resize 这几个操作。

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace mfweb::util {

template <class T, std::size_t N>
class small_vector {
    static_assert(std::is_trivially_copyable_v<T>, "small_vector 要求元素可平凡复制");
    static_assert(N > 0, "内联容量必须大于 0");

public:
    small_vector() noexcept = default;

    small_vector(const small_vector& other) { assign(other.data(), other.size_); }
    small_vector& operator=(const small_vector& other) {
        if (this != &other) {
            clear();
            assign(other.data(), other.size_);
        }
        return *this;
    }

    small_vector(small_vector&& other) noexcept { move_from(other); }
    small_vector& operator=(small_vector&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    ~small_vector() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    [[nodiscard]] T* data() noexcept { return heap_ != nullptr ? heap_ : inline_ptr(); }
    [[nodiscard]] const T* data() const noexcept {
        return heap_ != nullptr ? heap_ : inline_ptr();
    }

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + size_; }
    [[nodiscard]] const T* end() const noexcept { return data() + size_; }

    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data()[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data()[i]; }
    [[nodiscard]] T& back() noexcept { return data()[size_ - 1]; }
    [[nodiscard]] const T& back() const noexcept { return data()[size_ - 1]; }

    void clear() noexcept { size_ = 0; }

    void push_back(const T& v) {
        ensure_capacity(size_ + 1);
        data()[size_++] = v;
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        ensure_capacity(size_ + 1);
        T* slot = data() + size_;
        *slot = T{std::forward<Args>(args)...};  // T 可平凡复制，构造后赋值即可
        ++size_;
        return *slot;
    }

    void pop_back() noexcept {
        if (size_ > 0) { --size_; }
    }

    void resize(std::size_t n) {
        if (n > size_) { ensure_capacity(n); }
        size_ = n;
    }

    void reserve(std::size_t n) { ensure_capacity(n); }

private:
    [[nodiscard]] T* inline_ptr() noexcept {
        return reinterpret_cast<T*>(static_cast<void*>(inline_));
    }
    [[nodiscard]] const T* inline_ptr() const noexcept {
        return reinterpret_cast<const T*>(static_cast<const void*>(inline_));
    }

    void release() noexcept {
        if (heap_ != nullptr) {
            ::operator delete(heap_);
            heap_ = nullptr;
        }
        cap_ = N;
        size_ = 0;
    }

    void move_from(small_vector& other) noexcept {
        if (other.heap_ != nullptr) {
            heap_ = other.heap_;
            cap_ = other.cap_;
            size_ = other.size_;
            other.heap_ = nullptr;
            other.cap_ = N;
            other.size_ = 0;
        } else {
            std::memcpy(inline_, other.inline_, sizeof(T) * other.size_);
            size_ = other.size_;
        }
    }

    void assign(const T* src, std::size_t n) {
        ensure_capacity(n);
        std::memcpy(data(), src, sizeof(T) * n);
        size_ = n;
    }

    void ensure_capacity(std::size_t need) {
        if (need <= cap_) { return; }
        std::size_t new_cap = cap_ * 2;
        if (new_cap < need) { new_cap = need; }
        auto* fresh = static_cast<T*>(::operator new(sizeof(T) * new_cap));
        std::memcpy(fresh, data(), sizeof(T) * size_);
        release_heap_only();
        heap_ = fresh;
        cap_ = new_cap;
    }

    void release_heap_only() noexcept {
        if (heap_ != nullptr) {
            ::operator delete(heap_);
            heap_ = nullptr;
        }
    }

    T* heap_ = nullptr;
    std::size_t cap_ = N;
    std::size_t size_ = 0;
    // **刻意不写 `{}`**：内联缓冲若零初始化，每次构造都要 memset 整个缓冲。
    // 实测过这个代价：route_params 的内联缓冲 256 字节 × 每请求一次构造，
    // 直接把单线程 QPS 从 20.3K 拉到 15.1K。
    // 只读 [0, size_) 区间的元素，未初始化部分是安全的。
    alignas(T) unsigned char inline_[sizeof(T) * N];
};

}  // namespace mfweb::util
