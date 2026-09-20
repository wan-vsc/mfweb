// HTTP 压测（对应简历项目描述里的 QPS 数字）。
//
// 为什么自带压测器：wrk 依赖 epoll/kqueue，Windows 上没有原生版本；把 wrk 放进
// 虚拟机打 Windows 服务端又会被 NAT 带宽卡死。所以 Windows 侧的主数字由本工具产出，
// 报告必须连带环境、参数与命令一起给出；Linux 侧另用原生 wrk 做交叉验证（P10）。
//
// 两个子命令分进程运行，避免客户端与服务端抢同一个事件循环：
//     mfbench http-server <port> [body_bytes]     固定响应的服务端
//     mfbench http-load   <port> <conns> <seconds> 并发压测客户端
//
// 负载模型：每条连接一个协程，**顺序请求-响应**（不发流水线请求），keep-alive 复用连接。
// 这测的是"并发连接数下的请求吞吐"，与 wrk 的默认行为一致，便于对照。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/http/server.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace mfweb::bench {
namespace {

using clock_type = std::chrono::steady_clock;

struct load_stats {
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> bytes{0};
};

[[nodiscard]] long long parse_arg(const char* text, long long fallback) {
    if (text == nullptr) { return fallback; }
    char* end = nullptr;
    const long long v = std::strtoll(text, &end, 10);
    return (end != text && v >= 0) ? v : fallback;
}

// 顺序请求-响应循环
coro::task<void> load_worker(runtime::io_context& ctx, std::uint16_t port, std::string request,
                             std::chrono::steady_clock::time_point deadline, load_stats* stats) {
    const io::native_socket s = net::make_client_socket(ctx);
    if (s == io::k_invalid_socket) {
        stats->errors.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    net::set_no_delay(s);

    const sockaddr_in addr = net::loopback_address(port);
    const auto cs = co_await coro::async_connect(ctx, s, reinterpret_cast<const sockaddr*>(&addr),
                                                 sizeof(addr));
    if (!cs.ok()) {
        net::close_socket(s);
        stats->errors.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    std::string acc;
    acc.reserve(4096);
    char chunk[8192];

    while (std::chrono::steady_clock::now() < deadline) {
        const auto ws = co_await coro::async_write(ctx, s, request.data(), request.size());
        if (!ws.ok()) {
            stats->errors.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // 读一个完整响应（按 Content-Length 判定边界）
        acc.clear();
        bool ok = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto rs = co_await coro::async_read(ctx, s, chunk, sizeof(chunk));
            if (!rs.ok()) {
                stats->errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (rs.bytes == 0) { break; }
            acc.append(chunk, rs.bytes);

            const std::size_t head_end = acc.find("\r\n\r\n");
            if (head_end == std::string::npos) { continue; }
            const std::size_t cl_pos = acc.find("Content-Length:");
            if (cl_pos == std::string::npos || cl_pos > head_end) {
                ok = true;  // 无 body
                break;
            }
            const std::size_t digits = cl_pos + 15;
            const std::size_t eol = acc.find("\r\n", digits);
            const char* first = acc.data() + digits;
            const char* last = acc.data() + (eol == std::string::npos ? acc.size() : eol);
            std::size_t want = 0;
            const auto [ptr, ec] = std::from_chars(first, last, want);
            if (ec != std::errc{}) {
                ok = true;
                break;
            }
            if (acc.size() >= head_end + 4 + want) {
                stats->bytes.fetch_add(acc.size(), std::memory_order_relaxed);
                ok = true;
                break;
            }
        }
        if (!ok) { break; }
        stats->completed.fetch_add(1, std::memory_order_relaxed);
    }

    net::close_socket(s);
}

void print_usage() {
    std::printf("用法:\n");
    std::printf("  mfbench http-server <port> [body_bytes] [threads]\n");
    std::printf("  mfbench http-load   <port> <conns> <seconds>\n");
}

}  // namespace

int run_http_server(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const auto port = static_cast<std::uint16_t>(parse_arg(argv[1], 19090));
    const auto body_bytes = static_cast<std::size_t>(parse_arg(argc > 2 ? argv[2] : nullptr, 128));
    const auto threads = static_cast<std::size_t>(parse_arg(argc > 3 ? argv[3] : nullptr, 1));

    runtime::io_context ctx{threads};
    if (!ctx.valid()) {
        std::printf("io_context 初始化失败\n");
        return 2;
    }

    const std::string payload(body_bytes, 'x');
    http::server srv{ctx};
    // 用非拥有 body：不再每请求复制 128 字节
    srv.routes().get("/hello", [&payload](const http::request&, router::route_params&,
                                          http::response& r) {
        r.status = 200;
        r.set("Content-Type", "text/plain");
        r.body_view = payload;
    });
    srv.routes().get("/", [&payload](const http::request&, router::route_params&,
                                     http::response& r) {
        r.status = 200;
        r.body_view = payload;
    });

    if (!srv.listen(port)) {
        std::printf("监听 %u 失败\n", port);
        return 2;
    }
    std::printf("mfweb 压测服务端已启动：http://127.0.0.1:%u/hello（%zu 字节响应，%zu 个事件循环线程）\n",
                port, body_bytes, ctx.thread_count());
    std::fflush(stdout);
    srv.run();
    return 0;
}

int run_http_load(int argc, char** argv) {
    if (argc < 4) {
        print_usage();
        return 1;
    }
    const auto port = static_cast<std::uint16_t>(parse_arg(argv[1], 19090));
    const auto conns = static_cast<long long>(parse_arg(argv[2], 1000));
    const auto seconds = static_cast<long long>(parse_arg(argv[3], 10));

    runtime::io_context ctx;
    if (!ctx.valid()) {
        std::printf("io_context 初始化失败\n");
        return 2;
    }

    const std::string request =
        "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: */*\r\n\r\n";

    load_stats stats;
    const auto deadline = clock_type::now() + std::chrono::seconds(seconds);

    for (long long i = 0; i < conns; ++i) {
        ctx.spawn(load_worker(ctx, port, request, deadline, &stats));
    }

    std::printf("bench,conns,seconds,completed,errors,bytes,qps\n");
    const auto start = clock_type::now();
    while (!ctx.stopped() && (clock_type::now() < deadline || ctx.pending_io() > 0)) {
        if (!ctx.run_once(true, 5)) { break; }
    }
    const double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();

    const std::uint64_t done = stats.completed.load();
    std::printf("http_load,%lld,%lld,%llu,%llu,%llu,%.0f\n", conns, seconds,
                static_cast<unsigned long long>(done),
                static_cast<unsigned long long>(stats.errors.load()),
                static_cast<unsigned long long>(stats.bytes.load()),
                elapsed > 0.0 ? static_cast<double>(done) / elapsed : 0.0);
    std::fprintf(stderr, "[http] 连接 %lld，耗时 %.3f s，错误 %llu\n", conns, elapsed,
                 static_cast<unsigned long long>(stats.errors.load()));
    return 0;
}

}  // namespace mfweb::bench
