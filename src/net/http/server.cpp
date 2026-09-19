#include <mfweb/net/http/server.hpp>

#include <mfweb/net/buffer.hpp>
#include <mfweb/net/http/file_io.hpp>
#include <mfweb/net/http/static_file.hpp>

#include <fstream>
#include <vector>

namespace mfweb::http {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

[[nodiscard]] std::string url_decode_target(std::string_view target) {
    // 目标里的路径部分原样传给 resolve_path（它会做 URL 解码），这里只拿原始串。
    return std::string(target);
}

}  // namespace

bool server::listen(std::uint16_t port, const char* bind_ip) {
    listener_ = net::make_listener(*ctx_, port, bind_ip);
    if (listener_ == io::k_invalid_socket) { return false; }
    ctx_->spawn(accept_loop(listener_, static_prefix_, static_root_));
    return true;
}

void server::serve_static(std::string url_prefix, std::string root) {
    // 保证前缀以 / 开头、目录不以 / 结尾（拼接时统一处理）
    if (!url_prefix.empty() && url_prefix.front() != '/') { url_prefix.insert(url_prefix.begin(), '/'); }
    static_prefix_ = std::move(url_prefix);
    static_root_ = std::move(root);
}

void server::run() { ctx_->run(); }

coro::task<void> server::accept_loop(io::native_socket listener, std::string prefix,
                                     std::string root) {
    for (;;) {
        const io::native_socket accepted = io::iocp_engine::make_socket();
        if (accepted == io::k_invalid_socket) { co_return; }
        ctx_->engine().attach(accepted);

        const auto st = co_await coro::async_accept(*ctx_, listener, accepted);
        if (!st.ok()) {
            net::close_socket(accepted);
            if (ctx_->stopped()) { co_return; }
            continue;
        }

        net::set_no_delay(accepted);
        ctx_->spawn(serve_connection(accepted, prefix, root));
    }
}

coro::task<void> server::serve_connection(io::native_socket s, std::string prefix,
                                          std::string root) {
    request_parser parser;
    net::buffer buf{8192};

    for (;;) {
        buf.ensure_writable(8192);
        const auto rs = co_await coro::async_read(*ctx_, s, buf.write_ptr(), buf.writable());
        if (!rs.ok() || rs.bytes == 0) { break; }  // 出错或对端关闭
        buf.commit(rs.bytes);

        bool keep_going = true;
        while (keep_going) {
            request req;
            std::size_t consumed = 0;
            const auto pr = parser.feed(buf.peek(), buf.readable(), req, consumed);

            if (pr == request_parser::result::need_more) { break; }

            if (pr == request_parser::result::error) {
                const response bad = make_error_response(400);
                co_await write_response(s, bad, false);
                keep_going = false;
                break;
            }

            buf.consume(consumed);

            const bool head_only = (req.method_ == method::head);
            const response resp = handle_request(req, prefix, root);
            const bool ok = co_await write_response(s, resp, head_only);
            if (!ok || !req.keep_alive) {
                keep_going = false;
                break;
            }
            parser.reset();
        }

        if (!keep_going) { break; }
    }

    net::close_socket(s);
}

response server::handle_request(const request& req, const std::string& prefix,
                                const std::string& root) const {
    // 仅支持 GET/HEAD
    if (req.method_ != method::get && req.method_ != method::head) {
        response r = make_error_response(405);
        r.set("Allow", "GET, HEAD");
        return r;
    }

    // 静态文件
    if (prefix.empty() || req.target.substr(0, prefix.size()) == prefix) {
        const std::string_view rel = prefix.empty()
                                         ? req.target
                                         : req.target.substr(prefix.size());
        std::string path;
        if (resolve_path(root, rel, path)) {
            file_info info;
            if (stat_file(path, info)) {
                return build_static_response(req, path, info,
                                             req.method_ == method::head);
            }
            response r = make_error_response(404);
            r.body = "Not Found: " + std::string(req.target);
            return r;
        }
        response r = make_error_response(403);
        r.body = "Forbidden";
        return r;
    }

    response r = make_error_response(404);
    r.body = "Not Found: " + std::string(req.target);
    return r;
}

coro::task<bool> server::write_response(io::native_socket s, const response& resp,
                                        bool head_only) {
    std::string head;
    head.reserve(512);
    head += "HTTP/1.1 " + std::to_string(resp.status) + " ";
    head += status_text(resp.status);
    head += "\r\n";
    head += "Server: ";
    head += kServerName;
    head += "\r\n";
    head += "Date: " + now_http_date() + "\r\n";
    for (const auto& [k, v] : resp.headers) {
        head += k + ": " + v + "\r\n";
    }
    head += "Connection: keep-alive\r\n\r\n";

    const auto wh = co_await coro::async_write(*ctx_, s, head.data(), head.size());
    if (!wh.ok()) { co_return false; }
    if (head_only) { co_return true; }

    if (resp.stream_file) {
        std::ifstream in = open_file(resp.file_path);
        if (!in) { co_return false; }
        in.seekg(static_cast<std::streamoff>(resp.file_offset));

        std::vector<char> chunk(kChunk);
        std::uint64_t remaining = resp.file_length;
        while (remaining > 0) {
            const std::size_t want =
                remaining < kChunk ? static_cast<std::size_t>(remaining) : kChunk;
            in.read(chunk.data(), static_cast<std::streamsize>(want));
            const std::size_t got = static_cast<std::size_t>(in.gcount());
            if (got == 0) { break; }
            const auto wr = co_await coro::async_write(*ctx_, s, chunk.data(), got);
            if (!wr.ok()) { co_return false; }
            remaining -= got;
        }
        co_return true;
    }

    if (!resp.body.empty()) {
        const auto wb = co_await coro::async_write(*ctx_, s, resp.body.data(), resp.body.size());
        co_return wb.ok();
    }
    co_return true;
}

}  // namespace mfweb::http
