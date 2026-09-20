// 对照基准：朴素阻塞式 HTTP 服务端（每连接一个线程）。
//
// 存在的唯一目的：给 IOCP proactor 服务端做 A/B 对照。
// 如果"每请求 2 次异步读写"的模型开销真的是瓶颈，那么阻塞版（同步 recv/send，
// 无线程间调度、无 IRP/完成包）在同样负载下应该明显更快 —— 或者相反。
//
// 这不是要替代 IOCP 实现：阻塞版每连接一个线程，**扩展不到十万级连接**。
// 它只是一把尺子。

#include <mfweb/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mfweb::bench {
namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t k_bad = INVALID_SOCKET;
void close_sock(sock_t s) { ::closesocket(s); }
#else
using sock_t = int;
constexpr sock_t k_bad = -1;
void close_sock(sock_t s) { ::close(s); }
#endif

[[nodiscard]] long long parse_arg(const char* t, long long d) {
    if (t == nullptr) { return d; }
    char* end = nullptr;
    const long long v = std::strtoll(t, &end, 10);
    return (end != t && v >= 0) ? v : d;
}

std::atomic<long long> g_served{0};
std::atomic<long long> g_conns{0};

// 每连接一个线程：阻塞 recv 到请求头结束，然后阻塞 send 响应
void serve_blocking(sock_t s, const std::string* response) {
    g_conns.fetch_add(1, std::memory_order_relaxed);
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    char buf[4096];
    std::string acc;
    acc.reserve(2048);

    for (;;) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { break; }
        acc.append(buf, static_cast<std::size_t>(n));

        // 简单起见：只要收到含空行的完整请求头就回应（负载端是顺序请求-响应）
        std::size_t head_end = acc.find("\r\n\r\n");
        while (head_end != std::string::npos) {
            const std::size_t consumed = head_end + 4;
            acc.erase(0, consumed);
            head_end = acc.find("\r\n\r\n");
            // 发送完整响应（可能多次 send，直到发完）
            std::size_t sent = 0;
            while (sent < response->size()) {
                const int w = ::send(s, response->data() + sent,
                                     static_cast<int>(response->size() - sent), 0);
                if (w <= 0) {
                    close_sock(s);
                    g_conns.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
                sent += static_cast<std::size_t>(w);
            }
            g_served.fetch_add(1, std::memory_order_relaxed);
        }
    }
    close_sock(s);
    g_conns.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace

int run_http_server_blocking(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: mfbench http-server-blocking <port> [body_bytes]\n");
        return 1;
    }
    const auto port = static_cast<std::uint16_t>(parse_arg(argv[1], 19100));
    const auto body_bytes = static_cast<std::size_t>(parse_arg(argc > 2 ? argv[2] : nullptr, 128));

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 失败\n");
        return 2;
    }
#endif

    const sock_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == k_bad) {
        std::printf("socket 失败\n");
        return 2;
    }
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                 sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, SOMAXCONN) != 0) {
        std::printf("bind/listen 失败\n");
        return 2;
    }

    const std::string payload(body_bytes, 'x');
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                                 std::to_string(body_bytes) +
                                 "\r\nConnection: keep-alive\r\n\r\n" + payload;

    std::printf("mfweb 阻塞式对照服务端已启动：http://127.0.0.1:%u/hello（%zu 字节，每连接一线程）\n",
                port, body_bytes);
    std::fflush(stdout);

    std::vector<std::thread> workers;
    for (;;) {
        const sock_t s = ::accept(listener, nullptr, nullptr);
        if (s == k_bad) { break; }
        workers.emplace_back([s, &response] { serve_blocking(s, &response); });
        if ((g_conns.load() % 256) == 0) {
            // 回收已结束的线程，避免 vector 无限增长
            for (auto it = workers.begin(); it != workers.end();) {
                it = it->joinable() && false ? workers.erase(it) : it + 1;
            }
        }
    }
    return 0;
}

}  // namespace mfweb::bench
