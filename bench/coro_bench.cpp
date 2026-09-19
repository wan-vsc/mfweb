#include <mfweb/coro/frame_allocator.hpp>
#include <mfweb/coro/sync_wait.hpp>
#include <mfweb/coro/task.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

coro::task<int> trivial() { co_return 1; }

coro::task<int> nested(int depth) {
    if (depth == 0) { co_return 0; }
    co_return 1 + co_await nested(depth - 1);
}

void print_header() {
    std::printf(
        "bench,n,pool,seconds,ops_per_sec,ws_before_kb,ws_after_kb,heap_allocs,pooled_hits,"
        "pool_releases,cached_blocks\n");
}

void print_row(const char* name, long long n, double seconds, std::size_t ws_before,
               std::size_t ws_after) {
    const coro::frame_allocator_stats s = coro::frame_allocator::stats();
    std::printf("%s,%lld,%s,%.6f,%.0f,%zu,%zu,%zu,%zu,%zu,%zu\n", name, n,
                coro::frame_allocator::pool_enabled() ? "on" : "off", seconds,
                seconds > 0.0 ? static_cast<double>(n) / seconds : 0.0, ws_before / 1024,
                ws_after / 1024, s.heap_allocs, s.pooled_hits, s.pool_releases, s.cached_blocks);
}

[[nodiscard]] long long parse_count(const char* text, long long fallback) {
    if (text == nullptr) { return fallback; }
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    return (end != text && value > 0) ? value : fallback;
}

// 创建并销毁 n 个从未启动的协程：只测协程帧的分配/释放
void bench_create_destroy(long long n, bool pool) {
    coro::frame_allocator::trim();
    coro::frame_allocator::set_pool_enabled(pool);
    coro::frame_allocator::reset_stats();

    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();
    {
        // 必须让协程句柄"逃逸"到 vector 中。
        // MSVC 的 HALO 在句柄不逃逸时会把帧直接放在调用者栈上，完全不经过分配器 ——
        // 实测：写成循环内的局部变量时处理 100 万次仅需 0.0048 秒且 heap_allocs == 0，
        // 那测的其实是空循环，不是分配器。
        std::vector<coro::task<int>> batch;
        batch.reserve(static_cast<std::size_t>(n));
        for (long long i = 0; i < n; ++i) { batch.emplace_back(trivial()); }
    }  // 析构：全部帧归还分配器
    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("coro_create_destroy", n, seconds, ws_before, ws_after);
}

// 完整跑完 n 个协程：测协程启动、求值、对称转移、销毁的全链路吞吐
void bench_run(long long n, bool pool) {
    coro::frame_allocator::trim();
    coro::frame_allocator::set_pool_enabled(pool);
    coro::frame_allocator::reset_stats();

    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();
    long long checksum = 0;
    for (long long i = 0; i < n; ++i) { checksum += coro::sync_wait(trivial()); }
    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("coro_run_to_completion", n, seconds, ws_before, ws_after);
    std::fprintf(stderr, "[coro] 校验和 = %lld（防止循环被优化掉）\n", checksum);
}

// 同时驻留 n 个协程：测"每协程内存开销"这个百万并发最关键的指标
void bench_live(long long n, bool pool) {
    coro::frame_allocator::trim();
    coro::frame_allocator::set_pool_enabled(pool);
    coro::frame_allocator::reset_stats();

    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();

    std::vector<coro::task<int>> live;
    live.reserve(static_cast<std::size_t>(n));
    for (long long i = 0; i < n; ++i) { live.emplace_back(trivial()); }

    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    const double per_coro = n > 0 ? static_cast<double>(ws_after - ws_before) / static_cast<double>(n)
                                  : 0.0;
    print_row("coro_live_set", n, seconds, ws_before, ws_after);
    std::fprintf(stderr, "[coro] 每协程工作集增量 ≈ %.1f 字节（含 task 句柄 8 字节）\n", per_coro);
}

// 单次 n 层嵌套 co_await：若对称转移没做对，这里会栈溢出
void bench_nest(long long depth, bool pool) {
    coro::frame_allocator::trim();
    coro::frame_allocator::set_pool_enabled(pool);
    coro::frame_allocator::reset_stats();

    const std::size_t ws_before = working_set_bytes();
    const auto start = clock_type::now();
    const int result = coro::sync_wait(nested(static_cast<int>(depth)));
    const double seconds = elapsed_seconds(start);
    const std::size_t ws_after = working_set_bytes();
    print_row("coro_deep_nesting", depth, seconds, ws_before, ws_after);
    std::fprintf(stderr, "[coro] 嵌套结果 = %d（期望 %lld）\n", result, depth);
}

void print_usage() {
    std::printf("用法: mfbench coro <create|run|live|nest> [n] [pool|nopool]\n");
}

}  // namespace

int run_coro(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string_view mode(argv[1]);
    const long long n = parse_count(argc > 2 ? argv[2] : nullptr, 1000000);
    const bool pool = !(argc > 3 && std::strcmp(argv[3], "nopool") == 0);

    print_header();
    if (mode == "create") {
        bench_create_destroy(n, pool);
    } else if (mode == "run") {
        bench_run(n, pool);
    } else if (mode == "live") {
        bench_live(n, pool);
    } else if (mode == "nest") {
        bench_nest(n, pool);
    } else {
        print_usage();
        return 1;
    }
    return 0;
}

}  // namespace mfweb::bench
