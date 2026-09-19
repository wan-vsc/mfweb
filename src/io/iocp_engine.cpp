#include <mfweb/io/iocp_engine.hpp>

#include <atomic>
#include <cstring>

namespace mfweb::io {
namespace {

// WSAStartup 需要进程级引用计数：多个引擎可能同时存在
std::atomic<int> g_wsa_refs{0};

bool wsa_acquire() noexcept {
    if (g_wsa_refs.fetch_add(1) == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            g_wsa_refs.fetch_sub(1);
            return false;
        }
    }
    return true;
}

void wsa_release() noexcept {
    if (g_wsa_refs.fetch_sub(1) == 1) { WSACleanup(); }
}

constexpr ULONG_PTR k_wakeup_key = 1;
constexpr ULONG_PTR k_shutdown_key = 2;

}  // namespace

iocp_engine::iocp_engine() noexcept {
    if (!wsa_acquire()) {
        last_error_ = WSAGetLastError();
        return;
    }

    port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (port_ == nullptr) {
        last_error_ = static_cast<int>(GetLastError());
        wsa_release();
        return;
    }

    // 用一个临时套接字取出 AcceptEx / ConnectEx 的函数指针
    const native_socket probe = make_socket();
    if (probe == k_invalid_socket || !load_extensions(probe)) {
        if (probe != k_invalid_socket) { closesocket(probe); }
        CloseHandle(port_);
        port_ = nullptr;
        wsa_release();
        return;
    }
    closesocket(probe);
}

iocp_engine::~iocp_engine() {
    if (port_ != nullptr) {
        CloseHandle(port_);
        port_ = nullptr;
        wsa_release();
    }
}

native_socket iocp_engine::make_socket(int af) noexcept {
    return ::WSASocketW(af, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
}

bool iocp_engine::load_extensions(native_socket probe) noexcept {
    GUID accept_guid = WSAID_ACCEPTEX;
    GUID connect_guid = WSAID_CONNECTEX;
    DWORD bytes = 0;

    if (WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_guid, sizeof(accept_guid),
                 &accept_ex_, sizeof(accept_ex_), &bytes, nullptr, nullptr) != 0) {
        last_error_ = WSAGetLastError();
        return false;
    }
    if (WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_guid, sizeof(connect_guid),
                 &connect_ex_, sizeof(connect_ex_), &bytes, nullptr, nullptr) != 0) {
        last_error_ = WSAGetLastError();
        return false;
    }
    return true;
}

bool iocp_engine::attach(native_socket s) noexcept {
    if (port_ == nullptr || s == k_invalid_socket) { return false; }
    HANDLE h = CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), port_, 0, 0);
    if (h == nullptr) {
        last_error_ = static_cast<int>(GetLastError());
        return false;
    }
    return true;
}

bool iocp_engine::post_accept(io_operation& op, native_socket listener,
                              native_socket accepted) noexcept {
    op.kind = op_kind::accept;
    op.socket = accepted;
    op.aux = static_cast<std::uintptr_t>(listener);
    op.status = io_status{};

    constexpr DWORD kAddrLen = sizeof(SOCKADDR_STORAGE) + 16;
    DWORD received = 0;
    // 收数据长度传 0：连接一建立就完成，不等对端先发数据
    const BOOL ok = accept_ex_(listener, accepted, op.address_buffer, 0, kAddrLen, kAddrLen,
                               &received, &op.platform);
    if (ok == TRUE) { return true; }  // 立即完成，完成包仍会到达
    const int err = WSAGetLastError();
    if (err == WSA_IO_PENDING) { return true; }
    op.status.error = err;
    return false;
}

bool iocp_engine::post_connect(io_operation& op, native_socket s, const sockaddr* addr,
                               int addr_len) noexcept {
    op.kind = op_kind::connect;
    op.socket = s;
    op.status = io_status{};

    // ConnectEx 要求套接字已绑定到本地地址
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        op.status.error = WSAGetLastError();
        return false;
    }

    DWORD sent = 0;
    const BOOL ok = connect_ex_(s, addr, addr_len, nullptr, 0, &sent, &op.platform);
    if (ok == TRUE) { return true; }
    const int err = WSAGetLastError();
    if (err == WSA_IO_PENDING) { return true; }
    op.status.error = err;
    return false;
}

bool iocp_engine::post_read(io_operation& op, native_socket s, void* data,
                            std::size_t len) noexcept {
    op.kind = op_kind::read;
    op.socket = s;
    op.status = io_status{};

    WSABUF buf{};
    buf.buf = static_cast<char*>(data);
    buf.len = static_cast<ULONG>(len);
    DWORD flags = 0;
    DWORD received = 0;
    const int rc = ::WSARecv(s, &buf, 1, &received, &flags, &op.platform, nullptr);
    if (rc == 0) { return true; }
    const int err = WSAGetLastError();
    if (err == WSA_IO_PENDING) { return true; }
    op.status.error = err;
    return false;
}

bool iocp_engine::post_write(io_operation& op, native_socket s, const void* data,
                             std::size_t len) noexcept {
    op.kind = op_kind::write;
    op.socket = s;
    op.status = io_status{};

    WSABUF buf{};
    buf.buf = const_cast<char*>(static_cast<const char*>(data));
    buf.len = static_cast<ULONG>(len);
    DWORD sent = 0;
    const int rc = ::WSASend(s, &buf, 1, &sent, 0, &op.platform, nullptr);
    if (rc == 0) { return true; }
    const int err = WSAGetLastError();
    if (err == WSA_IO_PENDING) { return true; }
    op.status.error = err;
    return false;
}

bool iocp_engine::cancel(io_operation& op) noexcept {
    if (op.socket == k_invalid_socket) { return false; }
    // CancelIoEx 会为被取消的操作投递一个完成包（error = ERROR_OPERATION_ABORTED），
    // 因此调用方仍须等待完成，不能假定操作已结束。
    return ::CancelIoEx(reinterpret_cast<HANDLE>(op.socket), &op.platform) != 0;
}

void iocp_engine::finish(io_operation* op) noexcept {
    // accept 成功后必须把监听套接字上下文拷到新套接字上，
    // 否则 getsockname/getpeername 与后续 setsockopt 都不生效。
    if (op->kind == op_kind::accept && op->status.ok()) {
        const native_socket listener = static_cast<native_socket>(op->aux);
        ::setsockopt(op->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                     reinterpret_cast<const char*>(&listener), sizeof(listener));
    } else if (op->kind == op_kind::connect && op->status.ok()) {
        ::setsockopt(op->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
    }

    if (op->on_complete != nullptr) { op->on_complete(op); }
}

std::size_t iocp_engine::harvest(bool block, std::size_t max_events, unsigned timeout_ms) {
    if (port_ == nullptr) { return 0; }

    std::size_t handled = 0;
    DWORD timeout = block ? static_cast<DWORD>(timeout_ms) : 0;

    while (handled < max_events) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;

        const BOOL ok = ::GetQueuedCompletionStatus(port_, &bytes, &key, &overlapped, timeout);

        if (overlapped == nullptr) {
            // 没有完成包：要么超时，要么是唤醒包（wakeup 会带一个非空 overlapped）
            if (ok == FALSE) {
                const DWORD err = GetLastError();
                if (err == WAIT_TIMEOUT) { break; }
                last_error_ = static_cast<int>(err);
                break;
            }
            // overlapped == nullptr 且 ok == TRUE：不会发生，防御性退出
            break;
        }

        if (key == k_wakeup_key) { continue; }
        if (key == k_shutdown_key) { break; }

        auto* op = reinterpret_cast<io_operation*>(overlapped);

        if (ok == TRUE) {
            op->status.error = 0;
            op->status.bytes = bytes;
        } else {
            // 失败时 GetLastError() 就是该操作的错误码（这正是选用单发版的原因）
            op->status.error = static_cast<int>(GetLastError());
            op->status.bytes = bytes;
        }

        finish(op);
        ++handled;

        // 第一个完成包可以阻塞等，之后转为非阻塞，尽快把已有的都收干
        timeout = 0;
    }

    return handled;
}

void iocp_engine::wakeup() noexcept {
    if (port_ != nullptr) {
        ::PostQueuedCompletionStatus(port_, 0, k_wakeup_key, nullptr);
    }
}

}  // namespace mfweb::io
