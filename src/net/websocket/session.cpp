#include <mfweb/net/websocket/session.hpp>

#include <mfweb/net/socket.hpp>

#include <utility>

namespace mfweb::net::websocket {
namespace {

coro::task<void> ws_writer(runtime::io_context& ctx, io::native_socket s,
                           std::shared_ptr<ws_session::state_t> state) {
    for (;;) {
        if (state->out_queue.empty()) {
            if (state->closing) {
                // 收尾者：读循环已经退出，排空队列后由这里关 socket
                net::close_socket(s);
                co_return;
            }
            state->writer_active = false;
            co_return;  // 没有更多数据；下次 send 会重新拉起 writer
        }

        std::string frame = std::move(state->out_queue.front());
        state->out_queue.pop_front();

        const auto wr = co_await coro::async_write(ctx, s, frame.data(), frame.size());
        if (!wr.ok()) {
            net::close_socket(s);
            state->writer_active = false;
            co_return;
        }
    }
}

}  // namespace

void ws_session::send(opcode op, std::string_view payload) {
    state_->out_queue.push_back(make_frame(true, op, payload));
    if (!state_->writer_active) {
        state_->writer_active = true;
        ctx_->spawn(ws_writer(*ctx_, socket_, state_));
    }
}

void ws_session::close(close_code code) {
    if (state_->closing) { return; }
    state_->closing = true;
    send(opcode::close, make_close_payload(code));
}

coro::task<void> run_ws_session(runtime::io_context& ctx, io::native_socket s,
                                ws_handler handler) {
    ws_session session{ctx, s};
    handler(session);

    std::string buf;
    for (;;) {
        char chunk[4096];
        const auto rs = co_await coro::async_read(ctx, s, chunk, sizeof(chunk));
        if (!rs.ok() || rs.bytes == 0) { break; }  // 出错或对端断开
        buf.append(chunk, rs.bytes);

        bool peer_closed = false;
        for (;;) {
            frame_header h;
            std::size_t header_size = 0;
            const parse_status st = parse_frame_header(buf.data(), buf.size(), h, header_size);
            if (st == parse_status::need_more) { break; }
            if (st == parse_status::error) {
                session.close(close_code::protocol_error);
                buf.clear();
                peer_closed = true;
                break;
            }
            if (buf.size() < header_size + h.length) { break; }  // payload 未收齐

            char* payload = buf.data() + header_size;
            if (h.masked) { apply_mask(payload, h.length, h.mask_key); }
            const std::string_view view(payload, h.length);

            switch (h.op) {
                case opcode::text:
                case opcode::binary:
                    if (session.on_message) { session.on_message(session, h.op, view); }
                    break;
                case opcode::ping:
                    session.send_pong(view);
                    break;
                case opcode::pong:
                    break;  // 已发送 ping 的上层自行跟踪，这里只负责不透传
                case opcode::close:
                    session.close(close_code_of(view));  // 回 close 确认
                    peer_closed = true;
                    break;
                default:
                    session.close(close_code::protocol_error);
                    peer_closed = true;
                    break;
            }

            buf.erase(0, header_size + h.length);
            if (peer_closed) { break; }
        }
        if (peer_closed) { break; }
    }

    // 收尾：socket 恰好关一次。
    // writer 还在跑 → 让它排空队列后关；否则这里直接关。
    if (!session.state_->writer_active) {
        net::close_socket(s);
    } else {
        session.state_->closing = true;
    }
}

}  // namespace mfweb::net::websocket
