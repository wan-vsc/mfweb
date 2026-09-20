// qt_then_chain.cpp —— **真实 Qt 集成验证**（呼应简历第 5 条"便于集成至 Qt"）。
//
// 这个程序要证的不是"能编译"，而是：
//   1. mfweb 的 then 链能在一个**真实 QCoreApplication 事件循环**存在的情况下工作；
//   2. 最后一跳用 post_to_qt 把结果**投递回 Qt 主线程**，
//      并在回调里实测 QThread::currentThread() == qApp->thread() 来断言确实在主线程。
//
// 结构：
//   Qt 主线程      : QCoreApplication::exec()
//   mfweb 线程     : io_context 跑 http_client 的 then 链
//   最后一跳       : post_to_qt(&context, ...)  -> 回到 Qt 主线程
//
// 用法：qt_then_chain <host> <port> <path>

#include <mfweb/net/client/qt_executor.hpp>
#include <mfweb/net/http_client.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <QCoreApplication>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
int g_exit_code = 3;   // 默认：没跑到回调
bool g_on_main_thread = false;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (argc < 4) {
        std::printf("用法: qt_then_chain <host> <port> <path>\n");
        return 2;
    }
    const char* host = argv[1];
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const char* target = argv[3];

    // 代表一个 GUI 对象：它属于 Qt 主线程，post_to_qt 会投递到它所属的线程
    QObject context;
    std::printf("[Qt 主线程] 启动，主线程 id=%p\n",
                static_cast<void*>(QThread::currentThread()));

    mfweb::runtime::io_context ctx;
    if (!ctx.valid()) { std::printf("io_context 初始化失败\n"); return 2; }

    mfweb::net::http_client client{ctx, host, port};

    // 看门狗：万一没回调，别让 Qt 事件循环挂死
    QTimer::singleShot(10000, &app, [] {
        std::printf("[Qt 主线程] 超时：10 秒内没有收到回调\n");
        g_exit_code = 1;
        QCoreApplication::quit();
    });

    std::thread io_thread([&] {
        std::printf("[mfweb 线程] id=%p 发起请求 %s\n",
                    static_cast<void*>(QThread::currentThread()), target);

        client.get_async(target)
            .then([&context](mfweb::http::response& r) {
                const int status = r.status;
                std::string body = r.body;
                // 此刻在 mfweb 的 io_context 线程上 —— 绝不能碰 Qt 界面对象，
                // 必须投递回 Qt 主线程。
                mfweb::net::post_to_qt(&context, [status, body] {
                    const bool on_main =
                        QThread::currentThread() == QCoreApplication::instance()->thread();
                    g_on_main_thread = on_main;
                    std::printf("[Qt 主线程] 回调到达，线程 id=%p\n",
                                static_cast<void*>(QThread::currentThread()));
                    std::printf("[Qt 主线程] 状态码=%d 正文长度=%zu\n", status, body.size());
                    std::printf("[Qt 主线程] 实测在 Qt 主线程上: %s\n", on_main ? "是" : "否");
                    g_exit_code = (on_main && status == 200) ? 0 : 1;
                    QCoreApplication::quit();
                });
            })
            .on_error([&context](const std::error_code& ec) {
                const std::string msg = ec.message();
                mfweb::net::post_to_qt(&context, [msg] {
                    std::printf("[Qt 主线程] 请求失败: %s\n", msg.c_str());
                    g_exit_code = 1;
                    QCoreApplication::quit();
                });
            });

        // 驱动 mfweb 事件循环，直到 Qt 侧退出
        while (g_exit_code == 3) { ctx.run_once(true, 20); }
    });

    const int rc = app.exec();
    io_thread.join();

    std::printf("[Qt 主线程] app.exec() 返回 %d，最终结果 %s\n", rc,
                g_exit_code == 0 ? "通过" : "失败");
    return g_exit_code;
}
