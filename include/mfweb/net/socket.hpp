#pragma once

// mfweb::net::socket —— 套接字辅助函数。
//
// 只放"与平台打交道、与协议无关"的部分；连接级的状态机在 connection 里。

#include <mfweb/io/native_engine.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <cstdint>

namespace mfweb::net {

// listen(2) 的 backlog —— **不要直接用 SOMAXCONN**。
//
// Linux 把 SOMAXCONN 定义成 **4096**，而它就是"已完成连接队列"的长度。
// 高并发建连时（实测：167 个源 IP × 6000 条一次性涌入）4096 的队列会被瞬间冲爆，
// 内核直接丢包，客户端表现为"连上了又被断"，实测连接数死死卡在 86 万上不去。
// 这里显式给一个大值，内核最终会把它截到 net.core.somaxconn。
// Windows 侧 SOMAXCONN 是 0x7fffffff，同样接受这个值。
inline constexpr int k_listen_backlog = 65535;

}  // namespace mfweb::net

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mfweb::net {

inline void close_socket(io::native_socket s) noexcept {
#ifdef _WIN32
    if (s != io::k_invalid_socket) { ::closesocket(s); }
#else
    if (s != io::k_invalid_socket) { ::close(s); }
#endif
}

inline bool set_reuse_address(io::native_socket s) noexcept {
#ifdef _WIN32
    BOOL yes = TRUE;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                        sizeof(yes)) == 0;
#else
    int yes = 1;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0;
#endif
}

// 放大套接字发送/接收缓冲区。
//
// **为什么必须做**：一次 async_write 最多只能把发送缓冲区填满，之后要等完成事件才能再写。
// 于是单连接的吞吐 ≈ 缓冲区大小 ÷ 每次异步操作的往返延迟。
// 实测 Windows 默认发送缓冲区 64 KiB、每次异步操作约 70 µs：
//     64 KiB ÷ 70 µs ≈ 0.94 GB/s
// 而框架传 2 GiB 实测正好是 **0.94 GB/s** —— 完全吻合，说明瓶颈就在这里。
// 对照组：裸 TCP 用 4 MiB 缓冲区可跑到 3.67 GB/s。
//
// 注意这是在**每条连接**上设置，会影响内存占用（N 连接 × 缓冲区），
// 所以只在确认要传大文件的服务端连接上开，不当成全局默认。
inline bool set_socket_buffers(io::native_socket s, int bytes) noexcept {
#ifdef _WIN32
    const char* p = reinterpret_cast<const char*>(&bytes);
#else
    const void* p = &bytes;
#endif
    const bool a = ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, p, sizeof(bytes)) == 0;
    const bool b = ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, p, sizeof(bytes)) == 0;
    return a && b;
}

inline bool set_no_delay(io::native_socket s) noexcept {
#ifdef _WIN32
    BOOL yes = TRUE;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes),
                        sizeof(yes)) == 0;
#else
    int yes = 1;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
#endif
}

// 创建监听套接字：绑定到指定地址并 listen，同时关联到完成端口
[[nodiscard]] inline io::native_socket make_listener(runtime::io_context& ctx, std::uint16_t port,
                                                     const char* bind_ip = "127.0.0.1") noexcept {
    const io::native_socket s = io::native_engine::make_socket();
    if (s == io::k_invalid_socket) { return s; }

    set_reuse_address(s);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        close_socket(s);
        return io::k_invalid_socket;
    }

    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return io::k_invalid_socket;
    }
    if (::listen(s, k_listen_backlog) != 0) {
        close_socket(s);
        return io::k_invalid_socket;
    }
    if (!ctx.engine().attach(s)) {
        close_socket(s);
        return io::k_invalid_socket;
    }
    return s;
}

// 创建尚未连接的客户端套接字（已关联完成端口）。
// ConnectEx 要求套接字先绑定到本地地址 —— 这一步在 post_connect 里做。
[[nodiscard]] inline io::native_socket make_client_socket(runtime::io_context& ctx) noexcept {
    const io::native_socket s = io::native_engine::make_socket();
    if (s == io::k_invalid_socket) { return s; }
    if (!ctx.engine().attach(s)) {
        close_socket(s);
        return io::k_invalid_socket;
    }
    return s;
}

[[nodiscard]] inline sockaddr_in ipv4_address(const char* ip, std::uint16_t port) noexcept {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip == nullptr || ::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    }
    return addr;
}

[[nodiscard]] inline sockaddr_in loopback_address(std::uint16_t port) noexcept {
    return ipv4_address("127.0.0.1", port);
}

}  // namespace mfweb::net
