// 复现"连接超时导致的段错误"（简历第 2 条）。
//
// ⚠️ 本程序**故意采用有缺陷的朴素设计**，是一个缺陷复现夹具，不是可照抄的范例。
//    修正后的设计见 include/mfweb/net/connection.hpp，两者差异见 docs/bugs/asan-connection-timeout.md。
//
// 缺陷核心（一句话）：**在"有挂起 I/O 的情况下"销毁了协程帧。**
//
// 时序：
//   t=0      连接建立，读协程 co_await async_read，WSARecv 挂起
//   t=50ms   空闲超时触发，朴素代码做了三件"看起来没问题"的事：
//              closesocket + delete connection + 销毁读协程帧
//   t=50ms+  完成包到达事件循环 → 往已释放的协程帧里写状态并尝试恢复它
//             → heap-use-after-free
//
// 关键误区：closesocket/CancelIoEx 只是让挂起的操作**异步地**失败，完成包稍后才到；
// 在此之前绝不能销毁持有该操作对象的协程帧。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <chrono>
#include <coroutine>
#include <cstdio>

namespace {

using namespace std::chrono_literals;

// 手动驱动的最小根协程：能拿到句柄，从而能在挂起状态下显式销毁它
struct manual_root {
    struct promise_type {
        manual_root get_return_object() noexcept {
            return manual_root{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle{};
};

struct naive_connection {
    mfweb::runtime::io_context* ctx = nullptr;
    mfweb::io::native_socket socket = mfweb::io::k_invalid_socket;
    char buffer[1024]{};
};

manual_root naive_read(naive_connection* self) {
    const auto st = co_await mfweb::coro::async_read(*self->ctx, self->socket, self->buffer,
                                                     sizeof(self->buffer));
    (void)st;
    std::printf("[repro] 读协程正常恢复（若到达此处说明缺陷未被触发）\n");
}

manual_root naive_accept(mfweb::runtime::io_context& ctx, mfweb::io::native_socket listener,
                         mfweb::io::native_socket accepted) {
    co_await mfweb::coro::async_accept(ctx, listener, accepted);
}

mfweb::coro::task<void> idle_client(mfweb::runtime::io_context& ctx, std::uint16_t port,
                                    bool& connected) {
    const mfweb::io::native_socket s = mfweb::net::make_client_socket(ctx);
    const sockaddr_in addr = mfweb::net::loopback_address(port);
    const auto cs =
        co_await mfweb::coro::async_connect(ctx, s, reinterpret_cast<const sockaddr*>(&addr),
                                            sizeof(addr));
    connected = cs.ok();
    // 连上后故意什么都不发，让服务端空闲超时生效
}

}  // namespace

int main() {
    mfweb::runtime::io_context ctx;
    if (!ctx.valid()) {
        std::printf("[repro] io_context 初始化失败\n");
        return 2;
    }

    constexpr std::uint16_t kPort = 18951;
    const mfweb::io::native_socket listener = mfweb::net::make_listener(ctx, kPort);
    if (listener == mfweb::io::k_invalid_socket) {
        std::printf("[repro] 监听套接字创建失败\n");
        return 2;
    }

    // 1) 静默客户端
    bool connected = false;
    ctx.spawn(idle_client(ctx, kPort, connected));

    // 2) accept 一次
    const mfweb::io::native_socket accepted = mfweb::io::native_engine::make_socket();
    ctx.engine().attach(accepted);
    manual_root accept_root = naive_accept(ctx, listener, accepted);
    accept_root.handle.resume();

    // 3) 读协程：resume 到挂起（WSARecv 已提交）
    auto* conn = new naive_connection{&ctx, accepted, {}};
    manual_root read_root = naive_read(conn);
    read_root.handle.resume();
    std::printf("[repro] 读协程已挂起在 WSARecv 上\n");
    std::fflush(stdout);

    // 4) 空闲超时：朴素设计 —— 关套接字、删对象、销毁协程帧
    ctx.timers().schedule(mfweb::runtime::timer_queue::clock::now() + 50ms,
                          [conn, &read_root, &ctx] {
        std::printf("[repro] t=50ms 超时：closesocket + delete + 销毁读协程帧\n");
        std::fflush(stdout);
        ctx.engine().cancel_socket(conn->socket);
        mfweb::net::close_socket(conn->socket);
        delete conn;                 // ← 读操作还挂在它上面
        read_root.handle.destroy();  // ← 缺陷核心：销毁仍持有挂起 I/O 的协程帧
    });

    // 5) 驱动事件循环：完成包到达 → 写已释放的帧
    std::printf("[repro] 驱动事件循环……\n");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        ctx.run_once(true, 20);
    }

    std::printf("[repro] 事件循环结束（若未崩溃说明缺陷没有触发）\n");
    mfweb::net::close_socket(listener);
    return 0;
}
