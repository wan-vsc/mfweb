// 连接层验证。
//
// 重点不是"能不能 echo"，而是**关闭路径的生命周期是否正确** ——
// 同一套场景在朴素实现下会 use-after-free（见 tests/timeout_repro.cpp）。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/connection.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/test/test.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct echo_server {
    mfweb::runtime::io_context* ctx = nullptr;
    mfweb::io::native_socket listener = mfweb::io::k_invalid_socket;
    std::vector<std::shared_ptr<mfweb::net::connection>> live;
    int target = 1;
    int accepted = 0;
    int closed = 0;
    std::chrono::milliseconds idle_timeout{2000};
    std::size_t bytes_echoed = 0;

    mfweb::coro::task<void> accept_loop() {
        while (accepted < target) {
            const mfweb::io::native_socket s = mfweb::io::iocp_engine::make_socket();
            if (s == mfweb::io::k_invalid_socket) { co_return; }
            ctx->engine().attach(s);

            const auto st = co_await mfweb::coro::async_accept(*ctx, listener, s);
            if (!st.ok()) {
                mfweb::net::close_socket(s);
                co_return;
            }
            ++accepted;

            auto conn = std::make_shared<mfweb::net::connection>(*ctx, s);
            conn->set_idle_timeout(idle_timeout);
            conn->set_message_handler([this](mfweb::net::connection& c, const char* data,
                                             std::size_t len) {
                bytes_echoed += len;
                c.send(data, len);
            });
            conn->set_close_handler([this](mfweb::net::connection&) { ++closed; });
            live.push_back(conn);
            conn->start();
        }
    }
};

mfweb::coro::task<void> echo_client(mfweb::runtime::io_context& ctx, std::uint16_t port,
                                    std::string payload, bool& done, bool& matched) {
    const mfweb::io::native_socket s = mfweb::net::make_client_socket(ctx);
    const sockaddr_in addr = mfweb::net::loopback_address(port);
    const auto cs =
        co_await mfweb::coro::async_connect(ctx, s, reinterpret_cast<const sockaddr*>(&addr),
                                            sizeof(addr));
    if (!cs.ok()) {
        mfweb::net::close_socket(s);
        done = true;
        co_return;
    }

    const auto ws = co_await mfweb::coro::async_write(ctx, s, payload.data(), payload.size());
    if (!ws.ok()) {
        mfweb::net::close_socket(s);
        done = true;
        co_return;
    }

    std::string got(payload.size(), '\0');
    std::size_t total = 0;
    while (total < payload.size()) {
        const auto rs =
            co_await mfweb::coro::async_read(ctx, s, got.data() + total, payload.size() - total);
        if (!rs.ok() || rs.bytes == 0) { break; }
        total += rs.bytes;
    }

    matched = (total == payload.size()) && (got == payload);
    mfweb::net::close_socket(s);
    done = true;
}

[[nodiscard]] bool drive_until(mfweb::runtime::io_context& ctx,
                               const std::function<bool()>& predicate,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        ctx.run_once(true, 10);
    }
    return predicate();
}

}  // namespace

MFW_TEST(connection, single_echo_roundtrip) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    constexpr std::uint16_t kPort = 18961;
    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    MFW_CHECK(listener != mfweb::io::k_invalid_socket);

    echo_server server{&ctx, listener, {}, 1, 0, 0, 2000ms, 0};
    ctx.spawn(server.accept_loop());

    bool done = false;
    bool matched = false;
    ctx.spawn(echo_client(ctx, kPort, "mfweb connection test", done, matched));

    MFW_CHECK_MSG(drive_until(ctx, [&] { return done && matched; }, 10s), "echo 往返未完成");
    MFW_CHECK(matched);
    MFW_CHECK_EQ(server.accepted, 1);

    mfweb::net::close_socket(listener);
}

MFW_TEST(connection, many_concurrent_echoes) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    constexpr std::uint16_t kPort = 18962;
    constexpr int kConnections = 50;

    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    MFW_CHECK(listener != mfweb::io::k_invalid_socket);

    echo_server server{&ctx, listener, {}, kConnections, 0, 0, 5000ms, 0};
    ctx.spawn(server.accept_loop());

    // 用 deque<bool> 而非 vector<bool>：后者是位压缩的特化，operator[] 返回代理
    // 而不是 bool&，无法按引用传给 echo_client。
    std::deque<bool> done(kConnections, false);
    std::deque<bool> matched(kConnections, false);
    for (int i = 0; i < kConnections; ++i) {
        ctx.spawn(echo_client(ctx, kPort, "payload-" + std::to_string(i), done[static_cast<std::size_t>(i)],
                              matched[static_cast<std::size_t>(i)]));
    }

    const bool finished = drive_until(
        ctx,
        [&] {
            for (int i = 0; i < kConnections; ++i) {
                if (!done[static_cast<std::size_t>(i)] ||
                    !matched[static_cast<std::size_t>(i)]) {
                    return false;
                }
            }
            return true;
        },
        20s);

    MFW_CHECK_MSG(finished, "并发 echo 未全部完成");
    MFW_CHECK_EQ(server.accepted, kConnections);
    MFW_CHECK_MSG(server.bytes_echoed >= static_cast<std::size_t>(kConnections),
                  "服务端没有回显足够的数据");

    mfweb::net::close_socket(listener);
}

MFW_TEST(connection, idle_timeout_closes_silent_connection) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    constexpr std::uint16_t kPort = 18963;
    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    MFW_CHECK(listener != mfweb::io::k_invalid_socket);

    echo_server server{&ctx, listener, {}, 1, 0, 0, 120ms, 0};
    ctx.spawn(server.accept_loop());

    bool connected = false;
    auto silent_client = [](mfweb::runtime::io_context& c, std::uint16_t port,
                            bool& ok) -> mfweb::coro::task<void> {
        const mfweb::io::native_socket s = mfweb::net::make_client_socket(c);
        const sockaddr_in addr = mfweb::net::loopback_address(port);
        const auto cs =
            co_await mfweb::coro::async_connect(c, s, reinterpret_cast<const sockaddr*>(&addr),
                                                sizeof(addr));
        ok = cs.ok();
        (void)cs;
    };
    ctx.spawn(silent_client(ctx, kPort, connected));

    // 连接建立后什么都不发，等服务端空闲超时把连接关掉
    const bool closed_by_timeout =
        drive_until(ctx, [&] { return server.closed > 0; }, 5s);

    MFW_CHECK_MSG(connected, "客户端未能连接");
    MFW_CHECK_MSG(closed_by_timeout, "空闲超时没有关闭连接");
    MFW_CHECK_EQ(server.closed, 1);

    // 关键断言：连接关闭后，超时定时器必须已经从队列里摘掉（RAII）。
    // 若这里非空，就说明定时器会在连接销毁后触发 —— 那正是段错误的成因。
    MFW_CHECK_MSG(ctx.timers().empty(), "连接关闭后超时定时器仍留在队列里");
    MFW_CHECK(ctx.timers().validate());

    mfweb::net::close_socket(listener);
}

MFW_TEST(connection, close_is_idempotent_and_disarms_timer) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    constexpr std::uint16_t kPort = 18964;
    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    MFW_CHECK(listener != mfweb::io::k_invalid_socket);

    echo_server server{&ctx, listener, {}, 1, 0, 0, 5000ms, 0};
    ctx.spawn(server.accept_loop());

    bool connected = false;
    ctx.spawn([&]() -> mfweb::coro::task<void> {
        const mfweb::io::native_socket s = mfweb::net::make_client_socket(ctx);
        const sockaddr_in addr = mfweb::net::loopback_address(kPort);
        const auto cs =
            co_await mfweb::coro::async_connect(ctx, s, reinterpret_cast<const sockaddr*>(&addr),
                                                sizeof(addr));
        connected = cs.ok();
        (void)cs;
    }());

    MFW_CHECK(drive_until(ctx, [&] { return connected && server.accepted == 1; }, 5s));
    MFW_CHECK_EQ(ctx.timers().size(), static_cast<std::size_t>(1));  // 空闲定时器已挂

    server.live.front()->close();
    MFW_CHECK(server.live.front()->closing());
    server.live.front()->close();  // 幂等，不应出问题

    MFW_CHECK(drive_until(ctx, [&] { return server.closed > 0; }, 5s));
    MFW_CHECK_MSG(ctx.timers().empty(), "close() 之后超时定时器应当被摘掉");
    MFW_CHECK(ctx.timers().validate());

    mfweb::net::close_socket(listener);
}
