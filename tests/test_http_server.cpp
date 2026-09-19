// HTTP 服务器端到端验证：静态文件 + Range 断点续传。
//
// 用自研客户端发原始 HTTP 请求，验证 200/206/416 与 Content-Range、body 内容。
// 测试文件放在系统临时目录（纯 ASCII 路径），规避中文路径编码问题；
// 中文路径下的服务已在交付说明 §4.10 用 curl 单独验证。

#include <mfweb/coro/io_await.hpp>
#include <mfweb/net/http/server.hpp>
#include <mfweb/net/socket.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/test/test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace {

using namespace std::chrono_literals;

// 用自研客户端发原始请求（Connection: close），读完整个响应
[[nodiscard]] std::string raw_http(mfweb::runtime::io_context& ctx, std::uint16_t port,
                                   const std::string& request) {
    std::string result;
    bool done = false;
    const mfweb::io::native_socket s = mfweb::net::make_client_socket(ctx);
    const sockaddr_in addr = mfweb::net::loopback_address(port);

    auto client = [&](mfweb::runtime::io_context& c) -> mfweb::coro::task<void> {
        const auto cs =
            co_await mfweb::coro::async_connect(c, s, reinterpret_cast<const sockaddr*>(&addr),
                                                sizeof(addr));
        if (!cs.ok()) {
            mfweb::net::close_socket(s);
            done = true;
            co_return;
        }
        (void)co_await mfweb::coro::async_write(c, s, request.data(), request.size());

        char buf[4096];
        for (;;) {
            const auto rs = co_await mfweb::coro::async_read(c, s, buf, sizeof(buf));
            if (!rs.ok() || rs.bytes == 0) { break; }
            result.append(buf, rs.bytes);
        }
        mfweb::net::close_socket(s);
        done = true;
    };
    ctx.spawn(client(ctx));

    // 请求完成即停，而不是空转到超时（早前版本空转 5 秒/次，把测试拖到 33 秒）
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done && std::chrono::steady_clock::now() < deadline) { ctx.run_once(true, 10); }
    return result;
}

// 提取响应体（\r\n\r\n 之后的部分）
[[nodiscard]] std::string_view body_of(const std::string& resp) {
    const std::size_t p = resp.find("\r\n\r\n");
    return (p == std::string::npos) ? std::string_view{} : std::string_view(resp).substr(p + 4);
}

// 从响应头里取某字段值
[[nodiscard]] std::string_view header_of(const std::string& resp, const char* name) {
    const std::size_t hdr_end = resp.find("\r\n\r\n");
    const std::string_view head(resp.data(), hdr_end == std::string::npos ? resp.size() : hdr_end);
    const std::size_t p = head.find(std::string(name) + ":");
    if (p == std::string_view::npos) { return {}; }
    std::size_t v = head.find(':', p) + 1;
    while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) { ++v; }
    const std::size_t e = head.find("\r\n", v);
    return head.substr(v, e - v);
}

[[nodiscard]] bool drive_until(mfweb::runtime::io_context& ctx,
                               const std::function<bool()>& pred,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) { ctx.run_once(true, 10); }
    return pred();
}

}  // namespace

MFW_TEST(http_server, serves_static_file_full_and_range) {
    // 建测试文件（系统临时目录，纯 ASCII 路径）
    const auto dir = std::filesystem::temp_directory_path() / "mfweb_http_test";
    std::filesystem::create_directories(dir);
    const std::string file_path = (dir / "pattern.bin").string();
    {
        std::ofstream f(file_path, std::ios::binary);
        for (int i = 0; i < 1000; ++i) { f.put(static_cast<char>('A' + (i % 26))); }
    }

    mfweb::runtime::io_context ctx;
    MFW_CHECK(ctx.valid());

    mfweb::http::server srv{ctx};
    srv.serve_static("/files", dir.string());
    MFW_CHECK_MSG(srv.listen(18993), "监听失败");

    // 1) 完整 GET
    const std::string full = raw_http(ctx, 18993,
        "GET /files/pattern.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    MFW_CHECK_MSG(full.rfind("HTTP/1.1 200", 0) == 0, "完整 GET 应为 200");
    MFW_CHECK_EQ(body_of(full).size(), static_cast<std::size_t>(1000));

    // 2) Range 100-200
    const std::string part = raw_http(ctx, 18993,
        "GET /files/pattern.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=100-200\r\n"
        "Connection: close\r\n\r\n");
    MFW_CHECK_MSG(part.rfind("HTTP/1.1 206", 0) == 0, "Range 请求应为 206");
    MFW_CHECK_EQ(header_of(part, "Content-Range"), std::string_view("bytes 100-200/1000"));

    const std::string_view range_body = body_of(part);
    MFW_CHECK_EQ(range_body.size(), static_cast<std::size_t>(101));
    MFW_CHECK_EQ(range_body.front(), 'W');  // 'A' + (100 % 26) = 'W'

    // 3) 越界 Range → 416
    const std::string bad = raw_http(ctx, 18993,
        "GET /files/pattern.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=2000-\r\n"
        "Connection: close\r\n\r\n");
    MFW_CHECK_MSG(bad.rfind("HTTP/1.1 416", 0) == 0, "越界 Range 应为 416");

    // 4) 404
    const std::string nope = raw_http(ctx, 18993,
        "GET /files/nope.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    MFW_CHECK_MSG(nope.rfind("HTTP/1.1 404", 0) == 0, "不存在的文件应为 404");

    // 5) HEAD 无 body
    const std::string head = raw_http(ctx, 18993,
        "HEAD /files/pattern.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    MFW_CHECK_MSG(head.rfind("HTTP/1.1 200", 0) == 0, "HEAD 应为 200");
    MFW_CHECK_EQ(body_of(head).size(), static_cast<std::size_t>(0));

    // 清理
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MFW_TEST(http_server, rejects_directory_traversal) {
    const auto dir = std::filesystem::temp_directory_path() / "mfweb_http_test2";
    std::filesystem::create_directories(dir);

    mfweb::runtime::io_context ctx;
    mfweb::http::server srv{ctx};
    srv.serve_static("/files", dir.string());
    MFW_CHECK(srv.listen(18994));

    // ../ 穿越 → 403（resolve_path 拒绝）
    const std::string resp = raw_http(ctx, 18994,
        "GET /files/..%2F..%2FWindows%2Fwin.ini HTTP/1.1\r\nHost: t\r\n"
        "Connection: close\r\n\r\n");
    MFW_CHECK_MSG(resp.rfind("HTTP/1.1 403", 0) == 0, "目录穿越应被拒绝（403）");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
