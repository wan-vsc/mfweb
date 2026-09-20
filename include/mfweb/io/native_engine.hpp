#pragma once

// mfweb::io::native_engine —— **平台 I/O 后端的统一别名**。
//
// 为什么要这一层：io_context / socket / 测试原先直接写死了 iocp_engine，
// 于是 Windows 后端成了整个框架的硬依赖，Linux 连编都编不过。
// 业务代码只应该依赖"后端这个概念"，而不是某个具体实现。
//
//   * Windows → iocp_engine   （完成端口，AcceptEx/ConnectEx + overlapped）
//   * Linux   → uring_engine  （io_uring，自研系统调用封装，不链接 liburing）
//
// 两个后端必须提供**同一套公开成员**，见各自头文件：
//   valid / last_error / make_socket / attach /
//   post_accept / post_connect / post_read / post_write /
//   cancel / cancel_socket / harvest / wakeup

#ifdef _WIN32
#include <mfweb/io/iocp_engine.hpp>
namespace mfweb::io {
using native_engine = iocp_engine;
}
#else
#include <mfweb/io/uring_engine.hpp>
namespace mfweb::io {
using native_engine = uring_engine;
}
#endif
