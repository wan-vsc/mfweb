#include <mfweb/net/connection.hpp>
#include <mfweb/net/socket.hpp>

#include <utility>

namespace mfweb::net {

connection::connection(runtime::io_context& ctx, io::native_socket socket) noexcept
    : ctx_(&ctx), socket_(socket) {}

connection::~connection() {
    // 简历第 3 条那套 RAII 的落点：
    // 连接对象被销毁时，空闲超时定时器必须一起摘掉。
    // 少了这一步，定时器到期时会去执行一个捕获了 this 的回调 —— 对象早已释放。
    disarm_idle_timer();

    // 若还有挂起 I/O，取消它们。正常情况下 close() 已经做过，这里是兜底。
    if (socket_ != io::k_invalid_socket) {
        ctx_->engine().cancel_socket(socket_);
        close_socket(socket_);
        socket_ = io::k_invalid_socket;
    }
}

void connection::start() {
    if (started_) { return; }
    started_ = true;
    // 读循环协程持有 shared_ptr：只要它还挂在 I/O 上，连接对象就不会被销毁。
    // 这保证了完成包到达时，操作对象所在的协程帧一定还活着。
    ctx_->spawn(read_loop(shared_from_this()));
    arm_idle_timer();
}

void connection::close() {
    if (closing_) { return; }
    closing_ = true;

    // 1) 先摘定时器：避免超时回调在收尾过程中再次触发
    disarm_idle_timer();

    // 2) 取消挂起的读。CancelIoEx 会让完成包带着 ERROR_OPERATION_ABORTED 到达，
    //    读循环被唤醒后自行收尾。**注意这里不销毁任何对象** ——
    //    直接销毁就成了 use-after-free（见 docs/bugs/asan-connection-timeout.md）。
    ctx_->engine().cancel_socket(socket_);
}

void connection::send(std::string_view data) { send(data.data(), data.size()); }

void connection::send(const void* data, std::size_t len) {
    if (closing_ || len == 0) { return; }
    write_queue_.emplace_back(static_cast<const char*>(data), len);
    if (!write_in_flight_) {
        write_in_flight_ = true;
        ctx_->spawn(flush_writes(shared_from_this()));
    }
}

void connection::arm_idle_timer() {
    if (idle_timeout_.count() <= 0 || closing_) { return; }
    disarm_idle_timer();
    idle_timer_ = ctx_->timers().schedule(
        runtime::timer_queue::clock::now() + idle_timeout_, [this] {
            idle_timer_.reset();  // 已被触发，句柄立即失效（句柄生命周期契约）
            close();
        });
}

void connection::disarm_idle_timer() noexcept {
    if (idle_timer_.valid()) { ctx_->timers().cancel(idle_timer_); }
}

coro::task<void> connection::read_loop(std::shared_ptr<connection> self) {
    for (;;) {
        if (closing_) { break; }

        arm_idle_timer();
        read_buffer_.ensure_writable(4096);

        const auto st = co_await coro::async_read(*ctx_, socket_, read_buffer_.write_ptr(),
                                                  read_buffer_.writable());

        disarm_idle_timer();

        if (!st.ok() || st.bytes == 0) { break; }  // 出错或对端关闭

        read_buffer_.commit(st.bytes);
        received_ += st.bytes;

        if (on_message_) { on_message_(*this, read_buffer_.peek(), read_buffer_.readable()); }
        read_buffer_.consume(read_buffer_.readable());
    }

    closing_ = true;
    disarm_idle_timer();
    if (socket_ != io::k_invalid_socket) {
        close_socket(socket_);
        socket_ = io::k_invalid_socket;
    }
    if (on_close_) { on_close_(*this); }
}

coro::task<void> connection::flush_writes(std::shared_ptr<connection> self) {
    while (!write_queue_.empty() && !closing_) {
        const std::string& front = write_queue_.front();
        const auto st = co_await coro::async_write(*ctx_, socket_, front.data(), front.size());
        if (!st.ok()) { break; }
        sent_ += st.bytes;
        write_queue_.pop_front();
    }
    write_in_flight_ = false;
}

}  // namespace mfweb::net
