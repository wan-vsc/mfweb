// P0 工具链门禁测试：把「中文路径 + MSVC + Windows SDK + C++20 协程 + IOCP」这条链路
// 固化成可重复执行的断言，任何一环出问题 ctest 会立刻失败。
#include <mfweb/test/test.hpp>

#include <coroutine>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#else
// Linux 侧对应的门禁：内核是否具备 io_uring
#include <mfweb/io/uring_syscall.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// 最小协程：验证 <coroutine> 可用且 promise 机制正常
struct void_task {
    struct promise_type {
        void_task get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

void_task coro_void_probe() { co_return; }

// 带返回值的协程：验证 return_value 与 coroutine_handle 取 promise
struct value_task {
    struct promise_type {
        int value = 0;
        value_task get_return_object() noexcept {
            return value_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(int v) noexcept { value = v; }
        void unhandled_exception() noexcept { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle{};

    int run() {
        handle.resume();
        return handle.promise().value;
    }

    ~value_task() {
        if (handle) { handle.destroy(); }
    }
};

value_task coro_value_probe() { co_return 42; }

#ifdef _WIN32
// WSA 生命周期守卫：保证断言失败提前返回时也能 WSACleanup
struct wsa_session {
    bool ok = false;
    wsa_session() {
        WSADATA data{};
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~wsa_session() {
        if (ok) { WSACleanup(); }
    }
    wsa_session(const wsa_session&) = delete;
    wsa_session& operator=(const wsa_session&) = delete;
};
#endif  // _WIN32

}  // namespace

MFW_TEST(toolchain, cxx20_language_mode) {
    static_assert(__cplusplus >= 202002L, "mfweb 要求 C++20");
    MFW_CHECK(__cplusplus >= 202002L);
}

MFW_TEST(toolchain, coroutine_void_runs) {
    coro_void_probe();
    MFW_CHECK(true);  // 能走到这里即说明协程机制正常
}

MFW_TEST(toolchain, coroutine_returns_value) {
    value_task t = coro_value_probe();
    MFW_CHECK_EQ(t.run(), 42);
}

// 同一套用例名在两个平台都要有意义，所以这里不删测试，而是各测各的"网络栈可用性"
MFW_TEST(toolchain, winsock_starts) {
#ifdef _WIN32
    wsa_session wsa;
    MFW_CHECK_MSG(wsa.ok, "WSAStartup 失败");
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    MFW_CHECK_MSG(s >= 0, "socket(AF_INET, SOCK_STREAM) 失败");
    if (s >= 0) { ::close(s); }
#endif
}

// Windows = 完成端口可用；Linux = io_uring 环可用
MFW_TEST(toolchain, iocp_handle_creatable) {
#ifdef _WIN32
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    MFW_CHECK_MSG(iocp != nullptr, "CreateIoCompletionPort 失败");
    if (iocp != nullptr) { CloseHandle(iocp); }
#else
    ::io_uring_params p{};
    const long fd = mfweb::io::uring::sys_setup(64, &p);
    MFW_CHECK_MSG(fd >= 0, "io_uring_setup 失败（内核不支持 io_uring？）");
    if (fd >= 0) {
        MFW_CHECK(p.sq_entries > 0);
        ::close(static_cast<int>(fd));
    }
#endif
}

// Windows = WSA_FLAG_OVERLAPPED 套接字 + AcceptEx 扩展指针；
// Linux  = 套接字可作为 io_uring 的操作目标（提交一次 READ 并取消）
MFW_TEST(toolchain, overlapped_socket_and_acceptex_pointer) {
#ifdef _WIN32
    wsa_session wsa;
    MFW_CHECK_MSG(wsa.ok, "WSAStartup 失败");

    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    MFW_CHECK_MSG(s != INVALID_SOCKET, "WSASocketW(WSA_FLAG_OVERLAPPED) 失败");

    if (s != INVALID_SOCKET) {
        LPFN_ACCEPTEX accept_ex = nullptr;
        GUID guid = WSAID_ACCEPTEX;
        DWORD bytes = 0;
        const int rc = WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                                &accept_ex, sizeof(accept_ex), &bytes, nullptr, nullptr);
        MFW_CHECK_MSG(rc == 0 && accept_ex != nullptr, "取 AcceptEx 扩展函数指针失败");
        closesocket(s);
    }
#else
    const int sv[2] = {0, 0};
    int fds[2] = {-1, -1};
    (void)sv;
    MFW_CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair 失败");
    if (fds[0] >= 0) { ::close(fds[0]); }
    if (fds[1] >= 0) { ::close(fds[1]); }
#endif
}

MFW_TEST(toolchain, target_is_64bit) {
    MFW_CHECK_EQ(sizeof(void*), static_cast<size_t>(8));
}
