#pragma once

// 协程帧分配器。
//
// 动机：百万并发下每个协程帧都是一次 operator new + operator delete，分配器会成为瓶颈。
//
// 做法：按 64 字节粒度分档，不超过 1 KiB 的帧走**线程局部 freelist** 复用；
//       超过阈值的帧直接走全局堆。关键细节：**入池的块一律按"档位大小"分配**，
//       而不是按请求大小分配 —— 否则同档位内的小块会被复用于大帧，造成越界写。
//
// 取舍：
//   * 牺牲少量常驻内存换分配吞吐；每档缓存块数有上限（kMaxCachedPerClass），
//     因为协程可能在 A 线程创建、B 线程销毁，无上限的线程局部缓存会持续膨胀。
//   * 过对齐（over-aligned）帧不走池，直接走全局堆的 aligned 版本。
//
// 基准对照：把 MFWEB_CORO_FRAME_POOL 定义为 0 可整体关闭池化（见 bench B7）。

#include <cstddef>

#ifndef MFWEB_CORO_FRAME_POOL
#define MFWEB_CORO_FRAME_POOL 1
#endif

namespace mfweb::coro {

struct frame_allocator_stats {
    std::size_t pooled_hits = 0;    // 命中线程局部 freelist
    std::size_t pooled_misses = 0;  // 可入池但池空，走了堆
    std::size_t heap_allocs = 0;    // 直接走堆（超阈值）
    std::size_t pool_releases = 0;  // 归还到 freelist
    std::size_t heap_frees = 0;     // 直接释放到堆
    std::size_t cached_blocks = 0;  // 当前线程缓存块数
};

class frame_allocator {
public:
    // 分配 size 字节的协程帧（可能返回比 size 更大的块）
    [[nodiscard]] static void* allocate(std::size_t size);

    // 归还；size 必须与 allocate 时传入的一致（协程机制保证这一点）
    static void deallocate(void* ptr, std::size_t size) noexcept;

    // 以下统计均为**当前线程**的
    [[nodiscard]] static frame_allocator_stats stats() noexcept;
    static void reset_stats() noexcept;
    static void trim() noexcept;  // 释放当前线程缓存的全部块

    // 运行期开关，用于基准做 A/B 对照（基准 B7 对比开池/关池的协程吞吐）。
    // 编译期整体关闭请定义 MFWEB_CORO_FRAME_POOL=0。
    static void set_pool_enabled(bool enabled) noexcept;
    [[nodiscard]] static bool pool_enabled() noexcept;
};

}  // namespace mfweb::coro
