// HTTP 客户端测试：协程风格与 then 风格跑同一条链路，服务端用自研 http::server。

#include <mfweb/net/http/server.hpp>
#include <mfweb/net/http_client.hpp>
#include <mfweb/runtime/io_context.hpp>
#include <mfweb/test/test.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace {

using namespace std::chrono_literals;
namespace http = mfweb::http;

[[nodiscard]] bool drive_until(mfweb::runtime::io_context& ctx,
                               const std::function<bool()>& pred,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) { ctx.run_once(true, 10); }
    return pred();
}

// 起一个测试服务端：/hello 返回固定文本，/echo/{name} 回显路径参数
void start_server([[maybe_unused]] mfweb::runtime::io_context& ctx, mfweb::http::server& srv,
                  std::uint16_t port) {
    srv.routes().get("/hello", [](const http::request&, mfweb::router::route_params&,
                                  http::response& r) { r.body = "hello from mfweb"; });
    srv.routes().get("/echo/{name}",
                     [](const http::request&, mfweb::router::route_params& p, http::response& r) {
                         r.body = std::string(p.get("name"));
                     });
    MFW_CHECK_MSG(srv.listen(port), "测试服务端监听失败");
}

}  // namespace

MFW_TEST(http_client, coroutine_style_get) {
    mfweb::runtime::io_context ctx;
    mfweb::http::server srv{ctx};
    start_server(ctx, srv, 19001);

    bool done = false;
    std::string body;
    int status = 0;

    ctx.spawn([&]() -> mfweb::coro::task<void> {
        mfweb::net::http_client client{ctx, "127.0.0.1", 19001};
        auto r = co_await client.get("/hello");
        if (r) {
            status = r.value().status;
            body = r.value().body;
        }
        done = true;
    }());

    MFW_CHECK_MSG(drive_until(ctx, [&] { return done; }, 10s), "协程风格请求未完成");
    MFW_CHECK_EQ(status, 200);
    MFW_CHECK_EQ(body, std::string("hello from mfweb"));
}

MFW_TEST(http_client, then_chain) {
    mfweb::runtime::io_context ctx;
    mfweb::http::server srv{ctx};
    start_server(ctx, srv, 19002);

    bool done = false;
    std::string final_text;

    mfweb::net::http_client client{ctx, "127.0.0.1", 19002};

    // then 链：response → 取 body → 拼后缀。整条链不写一个协程。
    client.get_async("/echo/world")
        .then([](http::response& r) { return r.body; })
        .then([](std::string& s) { return s + "!"; })
        .then([&](std::string& s) {
            final_text = s;
            done = true;
            return 0;
        })
        .on_error([&](const std::error_code&) { done = true; });

    MFW_CHECK_MSG(drive_until(ctx, [&] { return done; }, 10s), "then 链未完成");
    MFW_CHECK_EQ(final_text, std::string("world!"));
}

MFW_TEST(http_client, then_error_propagates_to_on_error) {
    mfweb::runtime::io_context ctx;
    // 造一个"确实没人监听"的端口：绑定并监听后立刻关闭套接字。
    // （注意不能用 server::stop() —— 那只停事件循环，监听套接字还开着，
    //   连接会成功建立然后一直等响应，测试会以超时告终。）
    const mfweb::io::native_socket tmp = mfweb::net::make_listener(ctx, 19003);
    MFW_CHECK(tmp != mfweb::io::k_invalid_socket);
    mfweb::net::close_socket(tmp);

    mfweb::net::http_client client{ctx, "127.0.0.1", 19003};

    bool then_ran = false;
    bool error_seen = false;
    client.get_async("/x")
        .then([&](http::response&) {
            then_ran = true;
            return 0;
        })
        .on_error([&](const std::error_code&) { error_seen = true; });

    MFW_CHECK(drive_until(ctx, [&] { return error_seen || then_ran; }, 10s));
    MFW_CHECK_MSG(error_seen, "连接失败应触发 on_error");
    MFW_CHECK_MSG(!then_ran, "失败时不应执行 then 链");
}

MFW_TEST(async_result, value_and_error_paths) {
    mfweb::net::async_result<int> r;
    MFW_CHECK(!r.ready());

    int got = 0;
    r.then([&](int& v) {
         got = v * 2;
         return got;
     })
        .on_error([](const std::error_code&) {});
    MFW_CHECK_MSG(!r.ready() || true, "");
    MFW_CHECK_EQ(got, 0);  // 尚未完成

    r.set_value(21);
    MFW_CHECK_EQ(got, 42);
    MFW_CHECK(r.ok());
    MFW_CHECK_EQ(r.value(), 21);
}

MFW_TEST(async_result, error_short_circuits_and_is_one_shot) {
    mfweb::net::async_result<int> r;
    bool then_ran = false;
    bool error_ran = false;
    r.then([&](int& v) {
         then_ran = true;
         return v;
     })
        .on_error([&](const std::error_code&) { error_ran = true; });

    r.set_error(std::make_error_code(std::errc::connection_refused));
    MFW_CHECK(!then_ran);
    MFW_CHECK(error_ran);

    // 一次性：二次 set 无效
    r.set_value(5);
    MFW_CHECK_MSG(!r.ok(), "完成后再次 set_value 不应改变状态");
}

MFW_TEST(async_result, continuation_added_after_completion_runs_immediately) {
    mfweb::net::async_result<int> r;
    r.set_value(7);

    int seen = 0;
    r.then([&](int& v) {
        seen = v;
        return 0;
    });
    MFW_CHECK_EQ(seen, 7);  // 已完成 → 立即执行
}
