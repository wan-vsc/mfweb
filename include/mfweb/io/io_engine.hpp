#pragma once

// mfweb::io —— 异步 I/O 引擎。
//
// 模型是 **proactor**：调用方提交一次操作（"把 n 字节读进这块缓冲区"），
// 完成后由事件循环唤醒等待它的协程。与 reactor（"可读了叫我，我自己读"）的区别在于
// **缓冲区必须存活到操作完成** —— 这条约束贯穿整个 I/O 层，
// 也是后面 connection 生命周期与"超时段错误"的出发点。

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
// **必须定义 NOMINMAX**：否则 <windows.h> 会把 min/max 定义成宏，
// 破坏本库（以及任何使用者代码）里的 std::max / numeric_limits<T>::max() / time_point::max()。
// 项目自身的 CMake 构建在 cmake/CompilerOptions.cmake 里定义了它，
// 但**把 mfweb 作为库引入自己工程的使用者不会** —— 所以在头文件里兜住。
// 这个疏漏是照 README 的快速上手示例、用裸 cl 命令编译时暴露的。
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>  // AcceptEx/ConnectEx 及其函数指针类型、SO_UPDATE_*_CONTEXT
#include <windows.h>
#else
struct sockaddr;
#endif

namespace mfweb::io {

#ifdef _WIN32
using native_socket = SOCKET;
inline constexpr native_socket k_invalid_socket = INVALID_SOCKET;
#else
using native_socket = int;
inline constexpr native_socket k_invalid_socket = -1;
#endif

// 一次异步操作的结果。error 为 0 表示成功，否则是平台原生错误码。
struct io_status {
    int error = 0;
    std::size_t bytes = 0;

    [[nodiscard]] bool ok() const noexcept { return error == 0; }
    [[nodiscard]] std::string message() const;
};

enum class op_kind : std::uint8_t { none = 0, accept, connect, read, write };

struct io_operation;

using operation_completion = void (*)(io_operation* op) noexcept;

// 所有异步操作的公共头。
//
// **布局约定（改动前必读）**：`platform` 必须是第一个成员，且本结构必须是标准布局
// （无虚函数、无基类、所有成员同一访问级别）。原因：IOCP 的完成包只给出 OVERLAPPED*，
// 我们依赖"标准布局对象首成员地址 == 对象地址"这条标准保证把它还原成 io_operation*。
// Linux 后端会把 io_uring 的 user_data 指向同一个对象，布局约定同样成立。
struct io_operation {
#ifdef _WIN32
    OVERLAPPED platform{};
#endif

    operation_completion on_complete = nullptr;
    io_status status{};
    op_kind kind = op_kind::none;
    native_socket socket = k_invalid_socket;
    std::coroutine_handle<> continuation{};
    void* user = nullptr;
    std::uintptr_t aux = 0;

#ifdef _WIN32
    // AcceptEx 需要一块地址缓冲区，且必须存活到操作完成。
    // 放在操作对象里，让它随持有者（通常是协程帧）一起存活。
    // 用 uint64 数组而不是加 alignas 的 char 数组：既拿到 8 字节自然对齐
    // （地址结构体需要），又不会触发 /W4 的 C4324「因对齐说明符而填充」警告。
    static constexpr std::size_t kAddressBufferBytes = 2 * (sizeof(SOCKADDR_STORAGE) + 16);
    std::uint64_t address_buffer[(kAddressBufferBytes + 7) / 8]{};
#endif
};

}  // namespace mfweb::io
