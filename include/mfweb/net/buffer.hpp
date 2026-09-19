#pragma once

// mfweb::net::buffer —— 字节缓冲区。
//
// 设计：单块连续内存 + 读/写游标（head/tail），读写区域不重叠。
//   * 读侧：peek()/consume() 直接暴露已读入数据，解析器可以零拷贝地扫；
//   * 写侧：write_ptr()/commit() 把可写区域交给异步读，避免先读到临时数组再拷贝。
// 空间不足时自动增长（倍增），空间碎片化（head 之后有大段已消费区域）时先做一次整理。
//
// 不做的事：不做环形回绕。环形缓冲的收益在"固定容量 + 高频小包"场景，
// 代价是每次读都要处理两段不连续内存；HTTP 解析器需要连续内存做 SIMD 扫描，
// 因此这里选择"整理 + 增长"，把连续性作为第一优先级。

#include <mfweb/util/assert.hpp>

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace mfweb::net {

class buffer {
public:
    explicit buffer(std::size_t initial_capacity = 4096) { storage_.resize(initial_capacity); }

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    buffer(buffer&&) noexcept = default;
    buffer& operator=(buffer&&) noexcept = default;

    [[nodiscard]] std::size_t readable() const noexcept { return tail_ - head_; }
    [[nodiscard]] std::size_t writable() const noexcept { return storage_.size() - tail_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

    [[nodiscard]] const char* peek() const noexcept { return storage_.data() + head_; }
    [[nodiscard]] char* peek_mutable() noexcept { return storage_.data() + head_; }

    [[nodiscard]] std::string_view view() const noexcept { return {peek(), readable()}; }

    // ---- 读侧
    void consume(std::size_t n) noexcept {
        MFWEB_ASSERT(n <= readable());
        head_ += n;
        if (head_ == tail_) { head_ = 0; tail_ = 0; }  // 读空即复位，避免游标无限右移
    }

    // 供解析器使用：期望消费 n 字节但先不移动游标时用
    [[nodiscard]] bool starts_with(std::string_view prefix) const noexcept {
        return readable() >= prefix.size() &&
               std::memcmp(peek(), prefix.data(), prefix.size()) == 0;
    }

    // ---- 写侧
    [[nodiscard]] char* write_ptr() noexcept { return storage_.data() + tail_; }

    void commit(std::size_t n) noexcept {
        MFWEB_ASSERT(n <= writable());
        tail_ += n;
    }

    // 确保至少有 n 字节可写；必要时整理或扩容
    void ensure_writable(std::size_t n) {
        if (writable() >= n) { return; }
        compact();
        if (writable() >= n) { return; }
        storage_.resize(storage_.size() + n - writable() + storage_.size() / 2);
    }

    void append(const void* data, std::size_t n) {
        ensure_writable(n);
        std::memcpy(write_ptr(), data, n);
        commit(n);
    }

    void append(std::string_view s) { append(s.data(), s.size()); }

    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
    }

    // 把已消费的前缀丢掉，让可写空间连续
    void compact() noexcept {
        if (head_ == 0) { return; }
        const std::size_t n = readable();
        if (n != 0) { std::memmove(storage_.data(), storage_.data() + head_, n); }
        head_ = 0;
        tail_ = n;
    }

private:
    std::vector<char> storage_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
};

}  // namespace mfweb::net
