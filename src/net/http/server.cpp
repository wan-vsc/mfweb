#include <mfweb/net/http/server.hpp>

#include <mfweb/net/buffer.hpp>
#include <mfweb/net/http/file_io.hpp>
#include <mfweb/net/http/static_file.hpp>

#include <cctype>
#include <charconv>
#include <ctime>
#include <fstream>
#include <vector>

namespace mfweb::http {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

[[nodiscard]] bool iequals_sv(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// 头字段值（大小写不敏感）是否等于 expected
[[nodiscard]] bool header_equals(const request& req, const char* name, const char* expected) {
    return iequals_sv(req.header(name), expected);
}

// 头字段值是否包含给定 token（逗号分隔，大小写不敏感）
[[nodiscard]] bool header_contains_token(const request& req, const char* name,
                                         const char* token) {
    const std::string_view value = req.header(name);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string_view part =
            (comma == std::string_view::npos) ? value.substr(start)
                                              : value.substr(start, comma - start);
        std::size_t b = 0, e = part.size();
        while (b < e && (part[b] == ' ' || part[b] == '\t')) { ++b; }
        while (e > b && (part[e - 1] == ' ' || part[e - 1] == '\t')) { --e; }
        if (iequals_sv(part.substr(b, e - b), token)) { return true; }
        if (comma == std::string_view::npos) { break; }
        start = comma + 1;
    }
    return false;
}

}  // namespace

bool server::listen(std::uint16_t port, const char* bind_ip) {
    listener_ = net::make_listener(*ctx_, port, bind_ip);
    if (listener_ == io::k_invalid_socket) { return false; }
    ctx_->spawn(accept_loop(listener_, static_prefix_, static_root_));
    return true;
}

void server::ws(std::string path, net::websocket::ws_handler handler) {
    if (!path.empty() && path.front() != '/') { path.insert(path.begin(), '/'); }
    ws_path_ = std::move(path);
    ws_handler_ = std::move(handler);
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
        const io::native_socket accepted = io::native_engine::make_socket();
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

            // WebSocket 升级（RFC 6455 §4.2）
            if (!ws_path_.empty() && req.target.substr(0, ws_path_.size()) == ws_path_ &&
                header_equals(req, "upgrade", "websocket") &&
                header_contains_token(req, "connection", "upgrade") &&
                !req.header("sec-websocket-key").empty()) {
                response r101;
                r101.status = 101;
                r101.set("Upgrade", "websocket");
                r101.set("Connection", "Upgrade");
                r101.set("Sec-WebSocket-Accept",
                         net::websocket::compute_accept_key(req.header("sec-websocket-key")));
                const bool ok = co_await write_response(s, r101, true);
                if (!ok) {
                    keep_going = false;
                    break;
                }
                co_await net::websocket::run_ws_session(*ctx_, s, ws_handler_);
                keep_going = false;
                break;
            }

            const bool head_only = (req.method_ == method::head);

            // 动态路由优先于静态文件
            {
                router::route_params params;
                const router::handler* matched = nullptr;
                const auto mr = routes_.match(req.method_, req.target, params, matched);
                if (mr == router::match_result::found) {
                    response dyn;
                    (*matched)(req, params, dyn);
                    const bool ok = co_await write_response(s, dyn, head_only);
                    if (!ok || !req.keep_alive) {
                        keep_going = false;
                        break;
                    }
                    parser.reset();
                    continue;
                }
                if (mr == router::match_result::method_not_allowed) {
                    response m405 = make_error_response(405);
                    m405.set("Allow", "GET, HEAD, POST, PUT, DELETE");
                    const bool ok = co_await write_response(s, m405, head_only);
                    if (!ok || !req.keep_alive) {
                        keep_going = false;
                        break;
                    }
                    parser.reset();
                    continue;
                }
            }

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
    // 直接往 head 里追加，不再用 to_string/now_http_date 造临时 string。
    // 热路径上每个响应原先要产生 5~6 次临时字符串构造，这里全部消掉。
    // 注意：head 必须是**局部变量** —— async_write 的 WSABUF 直接指向它的内存，
    // 挂起期间必须保持有效；改成共享缓冲会与同线程的其他连接互相覆盖。
    char numbuf[16]{};
    char datebuf[48]{};

    std::string head;
    head.reserve(384);
    head += "HTTP/1.1 ";
    {
        const auto [end, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), resp.status);
        head.append(numbuf, static_cast<std::size_t>(end - numbuf));
    }
    head += ' ';
    head += status_text(resp.status);
    head += "\r\nServer: ";
    head += kServerName;
    head += "\r\nDate: ";
    {
        std::tm tm{};
        const std::time_t now = std::time(nullptr);
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        std::strftime(datebuf, sizeof(datebuf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        head += datebuf;
    }
    head += "\r\n";
    for (const auto& [k, v] : resp.headers) {
        head += k;
        head += ": ";
        head += v;
        head += "\r\n";
    }
    if (resp.status == 101) {
        head += "Connection: Upgrade\r\n\r\n";  // WebSocket 升级响应
    } else {
        head += "Connection: keep-alive\r\n\r\n";
    }

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

    if (!resp.body_view.empty()) {  // 非拥有 body：零拷贝发送
        const auto wb =
            co_await coro::async_write(*ctx_, s, resp.body_view.data(), resp.body_view.size());
        co_return wb.ok();
    }
    if (!resp.body.empty()) {
        const auto wb = co_await coro::async_write(*ctx_, s, resp.body.data(), resp.body.size());
        co_return wb.ok();
    }
    co_return true;
}

}  // namespace mfweb::http
