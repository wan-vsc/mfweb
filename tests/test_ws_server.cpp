// WebSocket 端到端：握手（RFC 6455 §1.3 向量）→ 掩码文本回显 → ping/pong → close 握手。
// 客户端是自研的（测试内实现），只验证协议行为，不引入第三方。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/http/server.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/net/websocket/ws.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/test/test.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace {

using namespace std::chrono_literals;
namespace ws = mfweb::net::websocket;

// 构造客户端掩码帧
[[nodiscard]] std::string masked_frame(const char* payload, std::size_t len,
                                       std::uint32_t mask_key, ws::opcode op = ws::opcode::text,
                                       bool fin = true) {
    std::string out;
    const std::uint8_t b0 =
        static_cast<std::uint8_t>((fin ? 0x80u : 0u) | (static_cast<std::uint8_t>(op) & 0x0Fu));
    out.push_back(static_cast<char>(b0));
    out.push_back(static_cast<char>(0x80u | static_cast<std::uint8_t>(len)));
    out.push_back(static_cast<char>((mask_key >> 24) & 0xFF));
    out.push_back(static_cast<char>((mask_key >> 16) & 0xFF));
    out.push_back(static_cast<char>((mask_key >> 8) & 0xFF));
    out.push_back(static_cast<char>(mask_key & 0xFF));
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t key_byte =
            static_cast<std::uint8_t>((mask_key >> (8 * (3 - (i % 4)))) & 0xFF);
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key_byte));
    }
    return out;
}

[[nodiscard]] bool drive_until(mfweb::runtime::io_context& ctx,
                               const std::function<bool()>& pred,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) { ctx.run_once(true, 10); }
    return pred();
}

}  // namespace

MFW_TEST(websocket_server, handshake_echo_pingpong_close) {
    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    // 服务端：echo + 计数
    mfweb::http::server srv{ctx};
    int echoed = 0;
    srv.ws("/ws", [&echoed](ws::ws_session& session) {
        session.on_message = [&echoed](ws::ws_session& s, ws::opcode op, std::string_view payload) {
            ++echoed;
            if (op == ws::opcode::text) { s.send_text(payload); }
            else { s.send_binary(payload); }
        };
    });
    MFW_CHECK_MSG(srv.listen(18995), "监听失败");

    // 客户端状态
    std::string buf;
    bool accept_ok = false;
    bool echo_ok = false;
    bool pong_ok = false;
    bool close_ok = false;
    bool server_unmasked = true;
    bool done = false;

    auto client = [&](mfweb::runtime::io_context& c) -> mfweb::coro::task<void> {
        const mfweb::io::native_socket s = mfweb::net::make_client_socket(c);
        const sockaddr_in addr = mfweb::net::loopback_address(18995);

        const auto cs =
            co_await mfweb::coro::async_connect(c, s, reinterpret_cast<const sockaddr*>(&addr),
                                                sizeof(addr));
        if (!cs.ok()) { done = true; co_return; }

        auto read_some = [&]() -> mfweb::coro::task<bool> {
            char chunk[4096];
            const auto rs = co_await mfweb::coro::async_read(c, s, chunk, sizeof(chunk));
            if (!rs.ok() || rs.bytes == 0) { co_return false; }
            buf.append(chunk, rs.bytes);
            co_return true;
        };

        auto ensure = [&](std::size_t n) -> mfweb::coro::task<bool> {
            while (buf.size() < n) {
                if (!(co_await read_some())) { co_return false; }
            }
            co_return true;
        };

        // 1) 握手（RFC 6455 §1.3 的 key）
        const std::string handshake =
            "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        (void)co_await mfweb::coro::async_write(c, s, handshake.data(), handshake.size());

        // 读握手响应头
        while (buf.find("\r\n\r\n") == std::string::npos) {
            if (!(co_await read_some())) { done = true; co_return; }
        }
        const std::size_t head_end = buf.find("\r\n\r\n") + 4;
        const std::string head = buf.substr(0, head_end);
        accept_ok =
            head.rfind("HTTP/1.1 101", 0) == 0 &&
            head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
        buf.erase(0, head_end);

        // 2) 掩码文本帧 → 回显
        const std::string text_frame = masked_frame("hello ws", 8, 0x11223344u);
        (void)co_await mfweb::coro::async_write(c, s, text_frame.data(), text_frame.size());

        for (;;) {
            ws::frame_header h;
            std::size_t hs = 0;
            const auto st = ws::parse_frame_header(buf.data(), buf.size(), h, hs);
            if (st == ws::parse_status::need_more || buf.size() < hs + h.length) {
                if (!(co_await read_some())) { done = true; co_return; }
                continue;
            }
            if (st == ws::parse_status::error) { done = true; co_return; }

            const std::string payload(buf.data() + hs, h.length);
            if (h.masked) { server_unmasked = false; }  // 服务器帧严禁掩码
            buf.erase(0, hs + h.length);

            if (h.op == ws::opcode::text) {
                echo_ok = (payload == "hello ws");
            } else if (h.op == ws::opcode::pong) {
                pong_ok = (payload == "abc");
            } else if (h.op == ws::opcode::close) {
                close_ok = true;
                break;
            } else if (h.op == ws::opcode::ping) {
                (void)co_await mfweb::coro::async_write(
                    c, s, masked_frame(payload.data(), payload.size(), 0xABCDEF01u,
                                       ws::opcode::pong).data(), payload.size() + 6);
                continue;
            }

            // 3) 发 ping → 期望 pong
            if (echo_ok && !pong_ok) {
                const std::string ping = masked_frame("abc", 3, 0x55667788u, ws::opcode::ping);
                (void)co_await mfweb::coro::async_write(c, s, ping.data(), ping.size());
            }
            // 4) 发 close → 期望 close 回执
            if (pong_ok) {
                const std::string close = masked_frame("\x03\xe8", 2, 0x99887766u,
                                                       ws::opcode::close);
                (void)co_await mfweb::coro::async_write(c, s, close.data(), close.size());
            }
        }

        mfweb::net::close_socket(s);
        done = true;
    };
    ctx.spawn(client(ctx));

    MFW_CHECK_MSG(drive_until(ctx, [&] { return done; }, 10s), "WS 会话未在 10 秒内完成");
    MFW_CHECK_MSG(accept_ok, "101 握手或 Sec-WebSocket-Accept 校验失败");
    MFW_CHECK_MSG(echo_ok, "文本回显内容不符");
    MFW_CHECK_MSG(pong_ok, "ping 未收到 pong 应答");
    MFW_CHECK_MSG(close_ok, "close 未收到回执");
    MFW_CHECK_MSG(server_unmasked, "服务器帧带掩码位（违反 RFC 6455 §5.1）");
    MFW_CHECK_EQ(echoed, 1);
}

MFW_TEST(websocket_server, non_upgrade_request_gets_404) {
    mfweb::runtime::io_context ctx;
    mfweb::http::server srv{ctx};
    srv.ws("/ws", [](ws::ws_session&) {});
    MFW_CHECK(srv.listen(18996));

    // 普通 GET 到 ws 路径 → 404（不是 WS 升级就不处理）
    std::string buf;
    bool done = false;
    auto client = [&](mfweb::runtime::io_context& c) -> mfweb::coro::task<void> {
        const mfweb::io::native_socket s = mfweb::net::make_client_socket(c);
        const sockaddr_in addr = mfweb::net::loopback_address(18996);
        const auto cs =
            co_await mfweb::coro::async_connect(c, s, reinterpret_cast<const sockaddr*>(&addr),
                                                sizeof(addr));
        if (!cs.ok()) { done = true; co_return; }
        const std::string req = "GET /ws HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        (void)co_await mfweb::coro::async_write(c, s, req.data(), req.size());
        char chunk[2048];
        const auto rs = co_await mfweb::coro::async_read(c, s, chunk, sizeof(chunk));
        if (rs.ok()) { buf.assign(chunk, rs.bytes); }
        mfweb::net::close_socket(s);
        done = true;
    };
    ctx.spawn(client(ctx));

    MFW_CHECK(drive_until(ctx, [&] { return done; }, 10s));
    MFW_CHECK_MSG(buf.rfind("HTTP/1.1 404", 0) == 0, "非升级请求应得到 404");
}
