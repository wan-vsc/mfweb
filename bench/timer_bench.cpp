// 定时器基准（对应简历第 3 条"优化删除定时器效率"）。
//
// 对照的两条取消路径：
//   cancel-handle : Awaiter 缓存红黑树节点指针 → 取消无需查找（生产路径）
//   cancel-index  : 朴素做法，额外维护 id → 节点 的哈希索引 → 先查表再删除
//
// 两条路径必须用**完全相同的随机取消顺序**，否则比较没有意义。

#include <mfweb/runtime/timer_queue.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace mfweb::bench {
namespace {

using clock_type = std::chrono::steady_clock;
using timer_queue = runtime::timer_queue;

[[nodiscard]] double elapsed_seconds(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

[[nodiscard]] std::size_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::size_t>(counters.WorkingSetSize);
    }
#endif
    return 0;
}

void print_header() {
    std::printf("bench,n,mode,seconds,ops_per_sec,ws_before_kb,ws_after_kb\n");
}

void print_row(const char* name, long long n, const char* mode, double seconds,
               std::size_t ws_before, std::size_t ws_after) {
    std::printf("%s,%lld,%s,%.6f,%.0f,%zu,%zu\n", name, n, mode, seconds,
                seconds > 0.0 ? static_cast<double>(n) / seconds : 0.0, ws_before / 1024,
                ws_after / 1024);
}

[[nodiscard]] long long parse_count(const char* text, long long fallback) {
    if (text == nullptr) { return fallback; }
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    return (end != text && value > 0) ? value : fallback;
}

[[nodiscard]] std::vector<std::size_t> shuffled_order(std::size_t n) {
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937 rng(20260919);  // 固定种子：两条路径必须看到同一个顺序
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

void bench_insert(long long n) {
    timer_queue q;
    const auto base = timer_queue::time_point{};
    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();
    for (long long i = 0; i < n; ++i) {
        q.schedule(base + std::chrono::nanoseconds(i + 1), [] {});
    }
    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("timer_insert", n, "plain", seconds, ws_before, ws_after);
}

void bench_fire(long long n) {
    timer_queue q;
    const auto base = timer_queue::time_point{};
    for (long long i = 0; i < n; ++i) {
        q.schedule(base + std::chrono::nanoseconds(i + 1), [] {});
    }
    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();
    const std::size_t fired = q.fire_expired(base + std::chrono::hours(1));
    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("timer_fire_all", n, fired == static_cast<std::size_t>(n) ? "plain" : "MISMATCH",
              seconds, ws_before, ws_after);
}

void bench_cancel(long long n, bool use_index, const std::vector<std::size_t>& order) {
    timer_queue q;
    q.enable_id_index(use_index);  // 索引路径需要额外维护 id → 节点 的哈希表

    std::vector<timer_queue::handle> handles;
    handles.reserve(static_cast<std::size_t>(n));
    const auto base = timer_queue::time_point{};
    for (long long i = 0; i < n; ++i) {
        handles.push_back(q.schedule(base + std::chrono::nanoseconds(i + 1), [] {}));
    }

    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();

    std::size_t cancelled = 0;
    if (use_index) {
        for (const std::size_t idx : order) {
            if (q.cancel_by_id(static_cast<timer_queue::timer_id>(idx + 1))) { ++cancelled; }
        }
    } else {
        for (const std::size_t idx : order) {
            if (q.cancel(handles[idx])) { ++cancelled; }
        }
    }

    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("timer_cancel_random", n, use_index ? "index-lookup" : "handle-cached", seconds,
              ws_before, ws_after);
    if (cancelled != static_cast<std::size_t>(n)) {
        std::fprintf(stderr, "[timer] 警告：取消数量 %zu != n %lld\n", cancelled, n);
    }
    if (!q.empty()) { std::fprintf(stderr, "[timer] 警告：取消后队列非空\n"); }
}

void print_usage() {
    std::printf("用法: mfbench timer <insert|fire|cancel-handle|cancel-index> [n]\n");
}

}  // namespace

int run_timer(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string_view mode(argv[1]);
    const long long n = parse_count(argc > 2 ? argv[2] : nullptr, 1000000);

    print_header();
    if (mode == "insert") {
        bench_insert(n);
    } else if (mode == "fire") {
        bench_fire(n);
    } else if (mode == "cancel-handle") {
        bench_cancel(n, false, shuffled_order(static_cast<std::size_t>(n)));
    } else if (mode == "cancel-index") {
        bench_cancel(n, true, shuffled_order(static_cast<std::size_t>(n)));
    } else {
        print_usage();
        return 1;
    }
    return 0;
}

}  // namespace mfweb::bench
