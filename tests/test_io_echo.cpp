// IOCP 异步 I/O 的端到端验证：同一进程内跑一对协程，走真实的 TCP 回环。
//
// 这条测试覆盖的链路：io_context → IOCP 引擎 → AcceptEx/ConnectEx/WSARecv/WSASend
// → io_awaiter → 协程恢复。任何一环出错都跑不通。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/buffer.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/test/test.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

using mfweb::coro::async_accept;
using mfweb::coro::async_connect;
using mfweb::coro::async_read;
using mfweb::coro::async_write;

mfweb::coro::task<void> echo_server(mfweb::runtime::io_context& ctx,
                                    mfweb::io::native_socket listener, bool& served) {
    const mfweb::io::native_socket accepted = mfweb::io::iocp_engine::make_socket();
    MFW_CHECK(accepted != mfweb::io::k_invalid_socket);
    MFW_CHECK(ctx.engine().attach(accepted));

    const auto ar = co_await async_accept(ctx, listener, accepted);
    MFW_CHECK_MSG(ar.ok(), ar.message().c_str());

    char buf[128];
    const auto rs = co_await async_read(ctx, accepted, buf, sizeof(buf));
    MFW_CHECK_MSG(rs.ok(), rs.message().c_str());
    MFW_CHECK(rs.bytes > 0);

    const auto ws = co_await async_write(ctx, accepted, buf, rs.bytes);
    MFW_CHECK_MSG(ws.ok(), ws.message().c_str());
    MFW_CHECK_EQ(ws.bytes, rs.bytes);

    mfweb::net::close_socket(accepted);
    served = true;
}

mfweb::coro::task<void> echo_client(mfweb::runtime::io_context& ctx, std::uint16_t port,
                                    bool& done) {
    const mfweb::io::native_socket s = mfweb::net::make_client_socket(ctx);
    MFW_CHECK(s != mfweb::io::k_invalid_socket);

    const sockaddr_in addr = mfweb::net::loopback_address(port);
    const auto cs =
        co_await async_connect(ctx, s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    MFW_CHECK_MSG(cs.ok(), cs.message().c_str());

    constexpr char kMessage[] = "hello mfweb";
    constexpr std::size_t kLen = sizeof(kMessage) - 1;

    const auto ws = co_await async_write(ctx, s, kMessage, kLen);
    MFW_CHECK_MSG(ws.ok(), ws.message().c_str());
    MFW_CHECK_EQ(ws.bytes, kLen);

    char buf[128];
    const auto rs = co_await async_read(ctx, s, buf, sizeof(buf));
    MFW_CHECK_MSG(rs.ok(), rs.message().c_str());
    MFW_CHECK_EQ(rs.bytes, kLen);
    MFW_CHECK(std::memcmp(buf, kMessage, kLen) == 0);

    mfweb::net::close_socket(s);
    done = true;
}

// 把事件循环驱动到条件满足或超时
[[nodiscard]] bool drive_until(mfweb::runtime::io_context& ctx,
                               const std::function<bool()>& predicate,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        ctx.run_once(true, 20);
    }
    return predicate();
}

}  // namespace

MFW_TEST(io, context_initializes) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK_MSG(ctx.valid(), "io_context 初始化失败（IOCP 或 Winsock 不可用）");
    MFW_CHECK_EQ(ctx.pending_io(), static_cast<std::size_t>(0));
}

MFW_TEST(io, tcp_echo_roundtrip) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK_MSG(ctx.valid(), "io_context 初始化失败");

    constexpr std::uint16_t kPort = 18931;
    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    MFW_CHECK_MSG(listener != mfweb::io::k_invalid_socket, "监听套接字创建失败");

    bool served = false;
    bool done = false;

    ctx.spawn(echo_server(ctx, listener, served));
    ctx.spawn(echo_client(ctx, kPort, done));

    MFW_CHECK_EQ(ctx.pending_io(), static_cast<std::size_t>(2));  // accept + connect 各一个

    const bool finished = drive_until(ctx, [&] { return done && served; },
                                      std::chrono::milliseconds(10000));

    MFW_CHECK_MSG(done, "客户端未完成");
    MFW_CHECK_MSG(served, "服务端未完成");
    MFW_CHECK_MSG(finished, "echo 往返在 10 秒内没有完成");

    mfweb::net::close_socket(listener);

    // I/O 全部完成后工作量计数应当归零
    const bool drained = drive_until(ctx, [&] { return ctx.pending_io() == 0; },
                                     std::chrono::milliseconds(1000));
    MFW_CHECK_MSG(drained, "I/O 完成后 pending_io 没有归零，说明完成包漏收或计数不平衡");
}

MFW_TEST(io, connect_to_closed_port_fails_cleanly) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    // 找一个没有监听的端口：先建再关
    const mfweb::io::native_socket probe = mfweb::net::make_listener(ctx, 18932);
    MFW_CHECK(probe != mfweb::io::k_invalid_socket);
    mfweb::net::close_socket(probe);

    bool done = false;
    bool connect_failed = false;

    auto client = [](mfweb::runtime::io_context& c, std::uint16_t port, bool& d,
                     bool& failed) -> mfweb::coro::task<void> {
        const mfweb::io::native_socket s = mfweb::net::make_client_socket(c);
        const sockaddr_in addr = mfweb::net::loopback_address(port);
        const auto cs =
            co_await async_connect(c, s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        failed = !cs.ok();
        mfweb::net::close_socket(s);
        d = true;
    };

    ctx.spawn(client(ctx, 18932, done, connect_failed));
    const bool finished = drive_until(ctx, [&] { return done; }, std::chrono::milliseconds(5000));

    MFW_CHECK_MSG(finished, "连接失败路径没有在 5 秒内返回");
    MFW_CHECK_MSG(connect_failed, "连接到已关闭端口却报告成功");
}

MFW_TEST(net_buffer, grows_and_compacts) {
    mfweb::net::buffer buf{16};
    MFW_CHECK(buf.empty());
    MFW_CHECK_EQ(buf.readable(), static_cast<std::size_t>(0));

    buf.append("hello");
    MFW_CHECK_EQ(buf.readable(), static_cast<std::size_t>(5));
    MFW_CHECK_EQ(buf.view(), std::string_view("hello"));

    buf.consume(2);
    MFW_CHECK_EQ(buf.view(), std::string_view("llo"));

    for (int i = 0; i < 100; ++i) { buf.append("0123456789"); }
    MFW_CHECK_EQ(buf.readable(), static_cast<std::size_t>(3 + 1000));
    MFW_CHECK(buf.capacity() >= buf.readable());

    buf.clear();
    MFW_CHECK(buf.empty());
    MFW_CHECK_EQ(buf.view(), std::string_view());
}

MFW_TEST(net_buffer, write_ptr_and_commit) {
    mfweb::net::buffer buf{8};
    buf.ensure_writable(4);
    std::memcpy(buf.write_ptr(), "abcd", 4);
    buf.commit(4);
    MFW_CHECK_EQ(buf.view(), std::string_view("abcd"));
    MFW_CHECK(buf.starts_with("abc"));
    MFW_CHECK(!buf.starts_with("bcd"));
}
