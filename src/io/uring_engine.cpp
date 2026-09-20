#include <mfweb/io/uring_engine.hpp>
#include <mfweb/io/uring_syscall.hpp>

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <mutex>
#include <vector>

namespace mfweb::io {
namespace {

using namespace mfweb::io::uring;

// ---- 环的内存布局 ---------------------------------------------------------
// 内核只给偏移，布局由用户态自己算。全部是单生产者/单消费者无锁环形缓冲，
// 但**多个 worker 线程会并发提交**，所以 SQ 侧用互斥量保护（见 ring_state::mu）。
struct ring_state {
    int ring_fd = -1;
    unsigned entries = 0;

    void* sq_ring = nullptr;
    std::size_t sq_ring_sz = 0;
    void* cq_ring = nullptr;
    std::size_t cq_ring_sz = 0;
    ::io_uring_sqe* sqes = nullptr;
    std::size_t sqes_sz = 0;

    unsigned* sq_head = nullptr;
    unsigned* sq_tail = nullptr;
    unsigned* sq_mask = nullptr;
    unsigned* sq_entries = nullptr;
    unsigned* sq_array = nullptr;

    unsigned* cq_head = nullptr;
    unsigned* cq_tail = nullptr;
    unsigned* cq_mask = nullptr;
    ::io_uring_cqe* cqes = nullptr;

    int event_fd = -1;
    unsigned pending = 0;             // 已填进 SQ 但还没 enter 的条目数

    std::mutex mu;                    // 保护 SQ 获取与提交
    std::vector<io_operation*> ready;  // 收割缓冲，复用避免每次分配
};

// 从环里取一个 SQE。返回 nullptr 表示 SQ 满（调用方按"提交失败"处理）。
::io_uring_sqe* acquire(ring_state& r) noexcept {
    const unsigned tail = __atomic_load_n(r.sq_tail, __ATOMIC_RELAXED);
    if (tail + 1 - __atomic_load_n(r.sq_head, __ATOMIC_ACQUIRE) > *r.sq_entries) {
        return nullptr;
    }
    const unsigned idx = tail & *r.sq_mask;
    ::io_uring_sqe* sqe = &r.sqes[idx];
    std::memset(sqe, 0, sizeof(*sqe));
    r.sq_array[idx] = idx;
    ++r.pending;
    __atomic_store_n(r.sq_tail, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

// 把已填的 SQE 真正交给内核
void enter(ring_state& r) noexcept {
    if (r.pending == 0) {
        return;
    }
    const unsigned n = r.pending;
    r.pending = 0;
    sys_enter(r.ring_fd, n, 0, 0);
}

void fill_common(::io_uring_sqe* sqe, unsigned char opcode, native_socket s,
                 io_operation& op) noexcept {
    sqe->opcode = opcode;
    sqe->fd = s;
    sqe->user_data = reinterpret_cast<std::uint64_t>(&op);
}

}  // namespace

struct uring_engine::impl {
    ring_state r;
};

uring_engine::uring_engine() noexcept {
    impl_ = new (std::nothrow) impl{};
    if (impl_ == nullptr) {
        last_error_.store(ENOMEM, std::memory_order_relaxed);
        return;
    }
    auto& r = impl_->r;

    ::io_uring_params p{};
    const long fd = sys_setup(256, &p);
    if (fd < 0) {
        last_error_.store(errno, std::memory_order_relaxed);
        return;
    }
    r.ring_fd = static_cast<int>(fd);
    r.entries = p.sq_entries;

    r.sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r.cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(::io_uring_cqe);
    r.sqes_sz = p.sq_entries * sizeof(::io_uring_sqe);

    r.sq_ring = ::mmap(nullptr, r.sq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                       r.ring_fd, kOffSqRing);
    r.cq_ring = ::mmap(nullptr, r.cq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                       r.ring_fd, kOffCqRing);
    r.sqes = static_cast<::io_uring_sqe*>(::mmap(nullptr, r.sqes_sz, PROT_READ | PROT_WRITE,
                                                 MAP_SHARED | MAP_POPULATE, r.ring_fd, kOffSqes));
    if (r.sq_ring == MAP_FAILED || r.cq_ring == MAP_FAILED || r.sqes == MAP_FAILED) {
        last_error_.store(errno, std::memory_order_relaxed);
        return;
    }

    auto* sq = static_cast<char*>(r.sq_ring);
    auto* cq = static_cast<char*>(r.cq_ring);
    r.sq_head = reinterpret_cast<unsigned*>(sq + p.sq_off.head);
    r.sq_tail = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
    r.sq_mask = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask);
    r.sq_entries = reinterpret_cast<unsigned*>(sq + p.sq_off.ring_entries);
    r.sq_array = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
    r.cq_head = reinterpret_cast<unsigned*>(cq + p.cq_off.head);
    r.cq_tail = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
    r.cq_mask = reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
    r.cqes = reinterpret_cast<::io_uring_cqe*>(cq + p.cq_off.cqes);

    // 注册 eventfd：wakeup() 写它就能立刻打断阻塞中的 io_uring_enter(GETEVENTS)
    const int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd >= 0) {
        if (sys_register(r.ring_fd, IORING_REGISTER_EVENTFD, &efd, 1) == 0) {
            r.event_fd = efd;
        } else {
            ::close(efd);
        }
    }
}

uring_engine::~uring_engine() {
    if (impl_ == nullptr) {
        return;
    }
    auto& r = impl_->r;
    if (r.sqes != nullptr && r.sqes != MAP_FAILED) { ::munmap(r.sqes, r.sqes_sz); }
    if (r.cq_ring != nullptr && r.cq_ring != MAP_FAILED) { ::munmap(r.cq_ring, r.cq_ring_sz); }
    if (r.sq_ring != nullptr && r.sq_ring != MAP_FAILED) { ::munmap(r.sq_ring, r.sq_ring_sz); }
    if (r.event_fd >= 0) { ::close(r.event_fd); }
    if (r.ring_fd >= 0) { ::close(r.ring_fd); }
    delete impl_;
}

bool uring_engine::valid() const noexcept {
    return impl_ != nullptr && impl_->r.ring_fd >= 0 && impl_->r.sq_ring != MAP_FAILED &&
           impl_->r.sqes != MAP_FAILED;
}

native_socket uring_engine::make_socket(int af) noexcept {
    return ::socket(af, SOCK_STREAM, IPPROTO_TCP);
}

bool uring_engine::attach(native_socket) noexcept {
    // io_uring 在提交时直接带 fd，没有"关联完成端口"这一步；保留空实现以统一接口。
    return true;
}

bool uring_engine::post_accept(io_operation& op, native_socket listener,
                               native_socket accepted) noexcept {
    auto& r = impl_->r;
    std::lock_guard<std::mutex> lk(r.mu);
    ::io_uring_sqe* sqe = acquire(r);
    if (sqe == nullptr) {
        op.status = {EAGAIN, 0};
        return false;
    }
    fill_common(sqe, IORING_OP_ACCEPT, listener, op);
    op.kind = op_kind::accept;
    // ACCEPT 不接受"预创建套接字"（这点与 AcceptEx 不同），
    // 完成时我们会 dup2 到调用方给的 fd 上，接口对上层保持不变。
    op.socket = listener;
    op.aux = static_cast<std::uintptr_t>(accepted);
    enter(r);
    return true;
}

bool uring_engine::post_connect(io_operation& op, native_socket s, const sockaddr* addr,
                                int addr_len) noexcept {
    auto& r = impl_->r;
    std::lock_guard<std::mutex> lk(r.mu);
    ::io_uring_sqe* sqe = acquire(r);
    if (sqe == nullptr) {
        op.status = {EAGAIN, 0};
        return false;
    }
    fill_common(sqe, IORING_OP_CONNECT, s, op);
    op.kind = op_kind::connect;
    sqe->addr = reinterpret_cast<std::uint64_t>(addr);
    sqe->off = static_cast<std::uint64_t>(addr_len);
    op.socket = s;
    enter(r);
    return true;
}

bool uring_engine::post_read(io_operation& op, native_socket s, void* data,
                             std::size_t len) noexcept {
    auto& r = impl_->r;
    std::lock_guard<std::mutex> lk(r.mu);
    ::io_uring_sqe* sqe = acquire(r);
    if (sqe == nullptr) {
        op.status = {EAGAIN, 0};
        return false;
    }
    fill_common(sqe, IORING_OP_READ, s, op);
    op.kind = op_kind::read;
    sqe->addr = reinterpret_cast<std::uint64_t>(data);
    sqe->len = static_cast<unsigned>(len);
    op.socket = s;
    enter(r);
    return true;
}

bool uring_engine::post_write(io_operation& op, native_socket s, const void* data,
                              std::size_t len) noexcept {
    auto& r = impl_->r;
    std::lock_guard<std::mutex> lk(r.mu);
    ::io_uring_sqe* sqe = acquire(r);
    if (sqe == nullptr) {
        op.status = {EAGAIN, 0};
        return false;
    }
    fill_common(sqe, IORING_OP_WRITE, s, op);
    op.kind = op_kind::write;
    sqe->addr = reinterpret_cast<std::uint64_t>(data);
    sqe->len = static_cast<unsigned>(len);
    op.socket = s;
    enter(r);
    return true;
}

bool uring_engine::cancel(io_operation& op) noexcept {
    auto& r = impl_->r;
    std::lock_guard<std::mutex> lk(r.mu);
    ::io_uring_sqe* sqe = acquire(r);
    if (sqe == nullptr) {
        return false;
    }
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = reinterpret_cast<std::uint64_t>(&op);
#ifdef IORING_ASYNC_CANCEL_USERDATA
    sqe->cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
#endif
    sqe->user_data = kTagCancel;
    enter(r);
    // 和 IOCP 一样：取消是异步的，目标操作仍然会以一个完成事件结束（这里是 -ECANCELED）。
    return true;
}

bool uring_engine::cancel_socket(native_socket s) noexcept {
    // 与 IOCP 的 CancelIoEx 语义**不同**，这里用 shutdown 来保证挂起的读立刻醒来：
    //   * CancelIoEx → 挂起读以 ERROR_OPERATION_ABORTED 结束
    //   * shutdown   → 挂起读以 0 字节（EOF）结束，上层按"对端关闭"处理
    // 目的相同：让等待中的协程自己醒来走正常收尾路径（ADR-010），
    // 而不是去销毁一个还有 I/O 在飞的协程帧。
    return ::shutdown(s, SHUT_RDWR) == 0;
}

std::size_t uring_engine::harvest(bool block, std::size_t max_events, unsigned timeout_ms) {
    auto& r = impl_->r;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        enter(r);
    }

    // ---- 先看有没有已经就绪的完成事件 ----
    auto drain = [&]() -> std::size_t {
        r.ready.clear();
        unsigned head = __atomic_load_n(r.cq_head, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
        while (head != tail && r.ready.size() < max_events) {
            const ::io_uring_cqe* cqe = &r.cqes[head & *r.cq_mask];
            const std::uint64_t tag = cqe->user_data;
            const int res = cqe->res;
            ++head;
            if (is_internal(tag)) {
                continue;  // TIMEOUT / CANCEL 的内部完成，跳过
            }
            auto* op = reinterpret_cast<io_operation*>(static_cast<std::uintptr_t>(tag));
            if (res < 0) {
                op->status = {-res, 0};
            } else {
                op->status = {0, static_cast<std::size_t>(res)};
                if (op->kind == op_kind::accept) {
                    // 把内核给的新 fd 落到调用方预创建的那个 fd 上
                    const int target = static_cast<int>(op->aux);
                    if (::dup2(res, target) < 0) {
                        op->status = {errno, 0};
                        ::close(res);
                    } else {
                        ::close(res);
                        op->socket = target;
                    }
                }
            }
            r.ready.push_back(op);
        }
        __atomic_store_n(r.cq_head, head, __ATOMIC_RELEASE);
        return r.ready.size();
    };

    std::size_t got = drain();
    if (got > 0 || !block) {
        for (std::size_t i = 0; i < got; ++i) {
            io_operation* op = r.ready[i];
            if (op->on_complete != nullptr) {
                op->on_complete(op);
            }
        }
        return got;
    }

    // ---- 阻塞等待：用一个 TIMEOUT 操作给 enter(GETEVENTS) 设上限 ----
    ::__kernel_timespec ts{};
    ts.tv_sec = static_cast<long long>(timeout_ms / 1000u);
    ts.tv_nsec = static_cast<long long>(timeout_ms % 1000u) * 1000000LL;

    bool armed = false;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (timeout_ms > 0) {
            ::io_uring_sqe* sqe = acquire(r);
            if (sqe != nullptr) {
                sqe->opcode = IORING_OP_TIMEOUT;
                sqe->fd = -1;
                sqe->addr = reinterpret_cast<std::uint64_t>(&ts);
                sqe->len = 1;
                sqe->user_data = kTagTimeout;
                armed = true;
            }
        }
        enter(r);
    }

    // 这一步才是真正的"睡在完成队列上"
    sys_enter(r.ring_fd, 0, 1, IORING_ENTER_GETEVENTS);

    // 若这次醒来是因为真实完成（而不是超时），把还没触发的 TIMEOUT 撤掉
    {
        const unsigned head = __atomic_load_n(r.cq_head, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
        bool timeout_fired = false;
        for (unsigned i = head; i != tail; ++i) {
            if (r.cqes[i & *r.cq_mask].user_data == kTagTimeout) {
                timeout_fired = true;
                break;
            }
        }
        if (armed && !timeout_fired) {
            std::lock_guard<std::mutex> lk(r.mu);
            ::io_uring_sqe* sqe = acquire(r);
            if (sqe != nullptr) {
                sqe->opcode = IORING_OP_ASYNC_CANCEL;
                sqe->addr = kTagTimeout;
#ifdef IORING_ASYNC_CANCEL_USERDATA
                sqe->cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
#endif
                sqe->user_data = kTagCancel;
                enter(r);
            }
        }
    }

    got = drain();
    for (std::size_t i = 0; i < got; ++i) {
        io_operation* op = r.ready[i];
        if (op->on_complete != nullptr) {
            op->on_complete(op);
        }
    }
    return got;
}

void uring_engine::wakeup() noexcept {
    if (impl_ == nullptr || impl_->r.event_fd < 0) {
        return;
    }
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(impl_->r.event_fd, &one, sizeof(one));
}

}  // namespace mfweb::io

#endif  // !_WIN32
