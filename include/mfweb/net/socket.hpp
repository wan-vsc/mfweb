#pragma once

// mfweb::net::socket —— 套接字辅助函数。
//
// 只放"与平台打交道、与协议无关"的部分；连接级的状态机在 connection 里。

#include <mfweb/io/native_engine.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <cstdint>

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
    if (::listen(s, SOMAXCONN) != 0) {
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

[[nodiscard]] inline sockaddr_in loopback_address(std::uint16_t port) noexcept {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    return addr;
}

}  // namespace mfweb::net
