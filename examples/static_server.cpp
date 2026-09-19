// 静态文件服务器示例：把命令行目录挂到 /files，支持 Range 断点续传。
//
// 用法：static_server.exe <端口> <目录>
//   curl -r 0-99 http://127.0.0.1:8080/files/big.bin -o part.bin
//   wget -c http://127.0.0.1:8080/files/big.bin     （Linux/VM 内）

#include <mfweb/net/http/file_io.hpp>
#include <mfweb/net/http/server.hpp>
#include <mfweb/runtime/io_context.hpp>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

int main() {
#ifdef _WIN32
    // 关键：Windows 的 argv 按 ANSI 代码页(936/GBK)编码，中文路径会被读坏。
    // 用 CommandLineToArgvW 拿 UTF-16，再转成 UTF-8，保证与框架内部的 UTF-8 路径约定一致。
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
    int argc = _argc;
    char** wargv = _argv;
#endif

    if (argc < 3) {
        std::printf("用法: static_server <端口> <目录>\n");
        return 2;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(
#ifdef _WIN32
        mfweb::http::wide_to_utf8(wargv[1]).c_str()
#else
        wargv[1]
#endif
        ));

    const std::string root =
#ifdef _WIN32
        mfweb::http::wide_to_utf8(wargv[2]);
#else
        wargv[2];
#endif

#ifdef _WIN32
    LocalFree(wargv);
#endif

    mfweb::runtime::io_context ctx;
    if (!ctx.valid()) {
        std::printf("io_context 初始化失败\n");
        return 2;
    }

    mfweb::http::server server{ctx};
    server.serve_static("/files", root);
    if (!server.listen(port)) {
        std::printf("监听 %u 失败\n", port);
        return 2;
    }

    std::printf("mfweb 静态文件服务器已启动：http://127.0.0.1:%u/files/  ← %s\n", port,
                root.c_str());
    std::printf("按 Ctrl+C 停止。\n");
    server.run();
    return 0;
}
