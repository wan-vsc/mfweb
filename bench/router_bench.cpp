// 路由匹配基准（对应简历第 2 条"非递归结构提升性能"）。
//
// 对照方式：**同一棵路由树**，同一批目标路径，分别用
//   iterative : router::match()           显式栈 + 显式回溯
//   recursive : router::match_recursive() 递归下降
// 两者语义已由差分测试保证一致（test_router.cpp）。

#include <mfweb/router/router.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace mfweb::bench {
namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] double elapsed_seconds(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

[[nodiscard]] long long parse_count(const char* text, long long fallback) {
    if (text == nullptr) { return fallback; }
    char* end = nullptr;
    const long long v = std::strtoll(text, &end, 10);
    return (end != text && v > 0) ? v : fallback;
}

// 朴素基线：把路由表当线性表，逐条按段比对。
// 存在的意义是把"结构带来的收益"和"栈实现方式带来的差异"分开量化 ——
// 前者是数量级的，后者只有几个百分点。
struct linear_route {
    std::vector<std::string> segs;
    bool wildcard = false;
};

struct linear_table {
    std::vector<linear_route> routes;

    void add(std::string_view pattern) {
        linear_route r;
        std::size_t start = 0;
        while (start < pattern.size()) {
            const std::size_t slash = pattern.find('/', start);
            const std::size_t end = (slash == std::string_view::npos) ? pattern.size() : slash;
            if (end > start) {
                std::string_view seg = pattern.substr(start, end - start);
                if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
                    seg = seg.substr(1, seg.size() - 2);
                    if (seg.size() > 3 && seg.substr(seg.size() - 3) == "...") {
                        r.wildcard = true;
                    }
                    r.segs.emplace_back("{}");  // 参数段标记
                } else {
                    r.segs.emplace_back(seg);
                }
            }
            if (slash == std::string_view::npos) { break; }
            start = slash + 1;
        }
        routes.push_back(std::move(r));
    }

    [[nodiscard]] bool match(std::string_view target) const {
        std::vector<std::string_view> segs;
        std::size_t start = 0;
        while (start < target.size()) {
            const std::size_t slash = target.find('/', start);
            const std::size_t end = (slash == std::string_view::npos) ? target.size() : slash;
            if (end > start) { segs.push_back(target.substr(start, end - start)); }
            if (slash == std::string_view::npos) { break; }
            start = slash + 1;
        }

        for (const auto& r : routes) {
            if (r.segs.size() != segs.size() && !r.wildcard) { continue; }
            if (r.wildcard && segs.size() < r.segs.size()) { continue; }
            bool ok = true;
            for (std::size_t i = 0; i < r.segs.size(); ++i) {
                if (r.segs[i] == "{}") { continue; }
                if (i >= segs.size() || r.segs[i] != segs[i]) {
                    ok = false;
                    break;
                }
            }
            if (ok) { return true; }
        }
        return false;
    }
};

struct corpus {
    router::router table;
    linear_table linear;
    std::vector<std::string> targets;  // 约 80% 命中、20% 未命中
};

// 生成混合路由表：静态段 + 参数段 + 通配，深度 2..5
[[nodiscard]] corpus build_corpus(long long route_count, long long lookup_count) {
    static const char* kStatic[] = {"api", "v1", "v2", "users", "items", "orders",
                                    "files", "search", "admin", "list"};

    corpus c;
    std::vector<std::string> hit_targets;

    for (long long i = 0; i < route_count; ++i) {
        std::string pattern;
        std::string concrete;
        const int depth = 2 + static_cast<int>(i % 4);
        for (int d = 0; d < depth; ++d) {
            pattern += "/";
            concrete += "/";
            const long long pick = (i * 7 + d * 13) % 10;
            if (pick == 0 && d == depth - 1) {
                pattern += "{rest...}";
                concrete += kStatic[(i + d) % 10];
            } else if (pick <= 2) {
                pattern += "{p" + std::to_string(d) + "}";
                concrete += kStatic[(i + d * 3) % 10];
            } else {
                pattern += kStatic[(i + d) % 10];
                concrete += kStatic[(i + d) % 10];
            }
        }
        const std::string body = std::to_string(i);
        c.table.add(http::method::get, pattern,
                    [body](const http::request&, router::route_params&, http::response& r) {
                        r.body = body;
                    });
        c.linear.add(pattern);
        hit_targets.push_back(std::move(concrete));
    }

    // 目标路径池：80% 取自真实路由（命中），20% 是随机词（多为未命中）。
    // 早期版本两边独立生成，实测命中率 0% —— 那测的是"全未命中时遍历整棵树"，
    // 与真实负载（多数命中）完全不是一回事。
    c.targets.reserve(static_cast<std::size_t>(lookup_count));
    for (long long i = 0; i < lookup_count; ++i) {
        if ((i % 5) != 0 && !hit_targets.empty()) {
            c.targets.push_back(hit_targets[static_cast<std::size_t>(i) % hit_targets.size()]);
        } else {
            std::string path;
            const int depth = 2 + static_cast<int>((i * 3) % 4);
            for (int d = 0; d < depth; ++d) {
                path += "/";
                path += kStatic[(i * 5 + d * 3) % 10];
            }
            c.targets.push_back(std::move(path));
        }
    }
    return c;
}

void bench_match(const corpus& c, bool recursive, long long lookups) {
    router::route_params params;
    const router::handler* out = nullptr;
    std::size_t hits = 0;

    const auto start = clock_type::now();
    for (long long i = 0; i < lookups; ++i) {
        const std::string& target = c.targets[static_cast<std::size_t>(i) % c.targets.size()];
        const auto r = recursive ? c.table.match_recursive(http::method::get, target, params, out)
                                 : c.table.match(http::method::get, target, params, out);
        if (r == router::match_result::found) { ++hits; }
    }
    const double seconds = elapsed_seconds(start);

    std::printf("router_match,%lld,%s,%.6f,%.0f\n", lookups,
                recursive ? "recursive" : "iterative", seconds,
                seconds > 0.0 ? static_cast<double>(lookups) / seconds : 0.0);
    std::fprintf(stderr, "[router] %s 命中率 %.1f%%\n", recursive ? "recursive" : "iterative",
                 100.0 * static_cast<double>(hits) / static_cast<double>(lookups));
}

void bench_linear(const corpus& c, long long lookups) {
    std::size_t hits = 0;
    const auto start = clock_type::now();
    for (long long i = 0; i < lookups; ++i) {
        const std::string& target = c.targets[static_cast<std::size_t>(i) % c.targets.size()];
        if (c.linear.match(target)) { ++hits; }
    }
    const double seconds = elapsed_seconds(start);
    std::printf("router_match,%lld,%s,%.6f,%.0f\n", lookups, "linear-scan", seconds,
                seconds > 0.0 ? static_cast<double>(lookups) / seconds : 0.0);
    std::fprintf(stderr, "[router] linear-scan 命中率 %.1f%%\n",
                 100.0 * static_cast<double>(hits) / static_cast<double>(lookups));
}

void print_usage() {
    std::printf("用法: mfbench router [routes] [lookups]\n");
}

}  // namespace

int run_router(int argc, char** argv) {
    const long long routes = parse_count(argc > 1 ? argv[1] : nullptr, 1000);
    const long long lookups = parse_count(argc > 2 ? argv[2] : nullptr, 1000000);

    std::printf("bench,n,mode,seconds,ops_per_sec\n");
    const corpus c = build_corpus(routes, 4096);
    std::fprintf(stderr, "[router] 路由数 %lld，目标路径池 %zu\n", routes, c.targets.size());

    bench_linear(c, lookups);
    bench_match(c, false, lookups);
    bench_match(c, true, lookups);
    return 0;
}

}  // namespace mfweb::bench
