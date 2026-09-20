// then 链式调用示例（呼应简历第 5 条：便于集成至 Qt 等非协程框架）。
//
// 用法：then_chain.exe <主机> <端口> <路径>
//   例：then_chain.exe 127.0.0.1 18990 /files/hello.txt

#include <mfweb/net/http_client.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const auto port = static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 18990);
    const char* target = argc > 3 ? argv[3] : "/files/hello.txt";

    mfweb::runtime::io_context ctx;
    if (!ctx.valid()) {
        std::printf("io_context 初始化失败\n");
        return 2;
    }

    mfweb::net::http_client client{ctx, host, port};
    bool done = false;

    // 整条链没有 co_await —— 这正是"非协程框架也能用"的形态
    client.get_async(target)
        .then([](mfweb::http::response& r) {
            std::printf("[then 1] 状态码 %d，%zu 字节\n", r.status, r.body.size());
            return r.body;
        })
        .then([](std::string& body) {
            std::printf("[then 2] 正文前 80 字节：%.80s\n", body.c_str());
            return static_cast<int>(body.size());
        })
        .then([&done](int& size) {
            std::printf("[then 3] 长度 = %d\n", size);
            done = true;
            return 0;
        })
        .on_error([&done](const std::error_code& ec) {
            std::printf("[on_error] 请求失败：%s\n", ec.message().c_str());
            done = true;
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < deadline) { ctx.run_once(true, 20); }

    if (!done) {
        std::printf("超时\n");
        return 1;
    }
    return 0;
}
