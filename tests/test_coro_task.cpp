#include <mfweb/coro/frame_allocator.hpp>
#include <mfweb/coro/sync_wait.hpp>
#include <mfweb/coro/task.hpp>
#include <mfweb/test/test.hpp>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

mfweb::coro::task<int> answer() { co_return 42; }

mfweb::coro::task<void> nothing() { co_return; }

mfweb::coro::task<std::string> greet(std::string who) { co_return "hello " + who; }

// volatile 让编译器无法在编译期判定"必然抛异常"。
// 否则 /O2 + ASAN 下会把 MFW_CHECK_THROWS 的"未抛异常"分支判成不可达代码（C4702）。
volatile bool g_always_throw = true;

mfweb::coro::task<int> throws_runtime() {
    if (g_always_throw) { throw std::runtime_error("boom"); }
    co_return 0;
}

mfweb::coro::task<int> propagates_from_child() { co_return co_await throws_runtime(); }

mfweb::coro::task<int> add_via_child(int a) { co_return a + co_await answer(); }

// 深嵌套：验证对称转移不消耗栈。若实现是"层层 resume 返回"，10 万层会栈溢出。
mfweb::coro::task<int> deep(int n) {
    if (n == 0) { co_return 0; }
    co_return 1 + co_await deep(n - 1);
}

// 用于验证"创建后不 await 直接析构"不泄漏、不崩溃
mfweb::coro::task<int> never_awaited() {
    co_return 7;
}

struct lifetime_probe {
    static int alive;
    lifetime_probe() { ++alive; }
    lifetime_probe(const lifetime_probe&) { ++alive; }
    lifetime_probe(lifetime_probe&&) noexcept { ++alive; }
    lifetime_probe& operator=(const lifetime_probe&) = default;
    lifetime_probe& operator=(lifetime_probe&&) noexcept = default;
    ~lifetime_probe() { --alive; }
};

int lifetime_probe::alive = 0;

mfweb::coro::task<lifetime_probe> make_probe() { co_return lifetime_probe{}; }

mfweb::coro::task<int> uses_probe() {
    lifetime_probe p = co_await make_probe();
    (void)p;
    co_return 1;
}

}  // namespace

// ---------------------------------------------------------------- 基本行为

MFW_TEST(coro_task, returns_value) {
    MFW_CHECK_EQ(mfweb::coro::sync_wait(answer()), 42);
}

MFW_TEST(coro_task, returns_void) {
    mfweb::coro::sync_wait(nothing());
    MFW_CHECK(true);  // 能跑完即通过
}

MFW_TEST(coro_task, moves_arguments_into_frame) {
    MFW_CHECK_EQ(mfweb::coro::sync_wait(greet("mfweb")), std::string("hello mfweb"));
}

MFW_TEST(coro_task, is_lazy_until_awaited) {
    bool started = false;
    auto lazy = [&started]() -> mfweb::coro::task<int> {
        started = true;
        co_return 1;
    };

    auto t = lazy();
    MFW_CHECK_MSG(!started, "task 在创建时就执行了，惰性语义被破坏");
    const int v = mfweb::coro::sync_wait(std::move(t));
    MFW_CHECK_EQ(v, 1);
    MFW_CHECK_MSG(started, "task 被 await 后仍未执行");
}

MFW_TEST(coro_task, not_copyable_but_movable) {
    static_assert(!std::is_copy_constructible_v<mfweb::coro::task<int>>);
    static_assert(std::is_move_constructible_v<mfweb::coro::task<int>>);
    MFW_CHECK(true);
}

MFW_TEST(coro_task, destroyed_without_await_is_safe) {
    {
        auto t = never_awaited();
        MFW_CHECK(t.valid());
    }  // 未 await 直接析构：必须安全析构协程帧
    MFW_CHECK(true);
}

// ---------------------------------------------------------------- 嵌套与异常

MFW_TEST(coro_task, awaits_child_coroutine) {
    MFW_CHECK_EQ(mfweb::coro::sync_wait(add_via_child(8)), 50);
}

MFW_TEST(coro_task, exception_propagates_to_caller) {
    MFW_CHECK_THROWS(mfweb::coro::sync_wait(throws_runtime()));
}

MFW_TEST(coro_task, exception_propagates_through_chain) {
    MFW_CHECK_THROWS(mfweb::coro::sync_wait(propagates_from_child()));
}

MFW_TEST(coro_task, deep_nesting_is_stack_safe) {
    // 10 万层嵌套：对称转移下栈占用为 O(1)，可以正常返回
    constexpr int kDepth = 100000;
    MFW_CHECK_EQ(mfweb::coro::sync_wait(deep(kDepth)), kDepth);
}

// ---------------------------------------------------------------- 值对象生命周期

MFW_TEST(coro_task, value_object_destroyed_exactly_once) {
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
    {
        const int r = mfweb::coro::sync_wait(uses_probe());
        MFW_CHECK_EQ(r, 1);
    }
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
}

// ---------------------------------------------------------------- 帧分配器

MFW_TEST(coro_frame_allocator, reuses_small_frames) {
    using mfweb::coro::frame_allocator;

    // 关键前提：必须让协程句柄"逃逸"到 vector 里。
    // MSVC 实现了 HALO（Heap Allocation eLision Optimization）：当协程句柄不逃逸时，
    // 帧会被直接放在调用者栈上，完全不经过 operator new —— 此时统计自然全是 0。
    // 这不是分配器失效，而是编译器把分配优化掉了。
    constexpr int kCount = 64;
    auto make_batch = []() {
        std::vector<mfweb::coro::task<int>> batch;
        batch.reserve(kCount);
        for (int i = 0; i < kCount; ++i) { batch.emplace_back(answer()); }
        return batch;  // 析构时归还全部帧
    };

    frame_allocator::trim();
    frame_allocator::reset_stats();

    // 第一轮：池为空，必然有真实分配，析构后归还到池
    { auto batch = make_batch(); (void)batch; }
    const auto after_first = frame_allocator::stats();
    MFW_CHECK_MSG(after_first.heap_allocs > 0, "第一轮没有发生堆分配，统计异常");
    MFW_CHECK_MSG(after_first.pool_releases > 0, "帧未被归还到池");

    // 第二轮：应当命中池
    const auto before = frame_allocator::stats();
    { auto batch = make_batch(); (void)batch; }
    const auto after_second = frame_allocator::stats();
    MFW_CHECK_MSG(after_second.pooled_hits > before.pooled_hits, "第二轮没有命中帧池");

    frame_allocator::trim();
    MFW_CHECK_EQ(frame_allocator::stats().cached_blocks, static_cast<std::size_t>(0));
}

MFW_TEST(coro_frame_allocator, halo_may_elide_frame_allocation) {
    // 记录 MSVC 的 HALO 行为：句柄不逃逸时，协程帧可能被放在栈上，
    // 因此"每个 task 必然产生一次帧分配"这个假设是错的。
    // 本用例只断言"不崩溃"，把行为差异显式留档。
    using mfweb::coro::frame_allocator;

    frame_allocator::trim();
    frame_allocator::reset_stats();

    for (int i = 0; i < 64; ++i) {
        auto t = answer();
        (void)t;
    }
    const auto s = frame_allocator::stats();
    const std::size_t total = s.heap_allocs + s.pooled_hits + s.pooled_misses;

    // 无论是否被优化掉，都不允许出现"归还多于分配"这种记账错误
    MFW_CHECK(s.pool_releases + s.heap_frees <= total);
}

MFW_TEST(coro_frame_allocator, same_class_blocks_fit_full_class_size) {
    // 同档位（65..128 字节）内的块必须都能安全写入整档大小。
    // 该断言在 Release 下是"不越界"的功能验证；在 Asan 配置下才是真正的越界检测。
    using mfweb::coro::frame_allocator;

    frame_allocator::trim();

    constexpr std::size_t kLow = 65;
    constexpr std::size_t kHigh = 128;
    std::vector<std::pair<void*, std::size_t>> blocks;

    for (std::size_t size = kLow; size <= kHigh; ++size) {
        void* p = frame_allocator::allocate(size);
        MFW_CHECK(p != nullptr);
        std::memset(p, 0xAB, kHigh);
        blocks.emplace_back(p, size);
    }
    for (const auto& [p, size] : blocks) { frame_allocator::deallocate(p, size); }

    // 复用阶段：再次申请并写满整档
    for (std::size_t size = kHigh; size >= kLow; --size) {
        void* p = frame_allocator::allocate(size);
        MFW_CHECK(p != nullptr);
        std::memset(p, 0xCD, kHigh);
        frame_allocator::deallocate(p, size);
        if (size == kLow) { break; }
    }

    frame_allocator::trim();
}

MFW_TEST(coro_frame_allocator, large_frames_bypass_pool) {
    using mfweb::coro::frame_allocator;

    frame_allocator::trim();
    frame_allocator::reset_stats();

    void* p = frame_allocator::allocate(4096);  // 超过 1 KiB 阈值
    MFW_CHECK(p != nullptr);
    frame_allocator::deallocate(p, 4096);

    const auto s = frame_allocator::stats();
    MFW_CHECK_EQ(s.heap_frees, static_cast<std::size_t>(1));
    MFW_CHECK_EQ(s.pool_releases, static_cast<std::size_t>(0));
}
