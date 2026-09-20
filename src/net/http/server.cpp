// 诊断打点需要
#include <chrono>
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

// 文件分块发送的块大小。
//
// **这个常数直接决定大文件吞吐**：实测宿主 Windows/IOCP 上
// 2 GB 传输耗时 2.43 s，即每 64 KiB 块约 73 µs —— 与 HTTP 每请求延迟同量级，
// 说明瓶颈是**每次异步操作的固定开销**，而不是带宽。
// 加大块就能线性摊薄这个开销（Linux/io_uring 侧每块约 32 µs，所以同样块大小下快一倍多）。
constexpr std::size_t kChunk = 2 * 1024 * 1024;

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

// 把 [data, data+len) **完整**写出去。
//
// 为什么必须循环：proactor 的契约是"这次操作完成了"，**不是**"请求的字节都写完了"。
// 一次 async_write 完全可能只写出一部分（对端接收窗口小、发送缓冲区满都会这样）。
//
// 早先的代码把"从文件读到的字节数"直接当作"已写出的字节数"往下走，
// 于是**静默丢掉尾部**：
//   * Windows/IOCP + loopback 上 64 KiB 基本一次写完，这个缺陷从不暴露；
//   * Linux/io_uring 上稳定复现 —— 10 MiB 传输每次少 1~5 KB（零头不固定），
//     客户端表现为"响应永远收不全、连接挂死"，而服务端 CPU 为 0、CQ 里也没有待处理事件。
coro::task<bool> write_all(runtime::io_context& ctx, io::native_socket s, const char* data,
                           std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const auto r = co_await coro::async_write(ctx, s, data + off, len - off);
        if (!r.ok() || r.bytes == 0) { co_return false; }
        off += r.bytes;
    }
    co_return true;
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
    // 注：曾在此处放大套接字缓冲区（4 MiB），试图解决大文件发送慢的问题。
    // **实测零收益**（0.87 vs 0.94 GB/s，在噪声范围内），因此撤掉 ——
    // 它会抬高每条静态连接的缓冲区上限，与"海量连接"这个目标冲突。
    // 大文件吞吐的真实瓶颈仍未定位，详见 03_性能实测报告.md。
    request_parser parser;
    // 每连接读缓冲的**起步**大小。
    //
    // 为什么不能一上来就要 8 KiB：这是**每连接常驻**的开销。
    // 实测 49.6 万并发连接时服务端每连接约 9.4 KB，其中最大的一块就是这个缓冲
    // （208,892 连接时 RSS 1,941 MB）。按此外推，100 万连接需要约 9.4 GB，
    // 超过本机 7.9 GB 内存 —— 也就是说**光是这个常数就挡住了"百万并发"**。
    //
    // 取 2 KiB：典型 HTTP 请求头只有几百字节，2 KiB 足够；
    // 更大的请求由下面的 ensure_writable 按需扩容（行为与原来 8192 时完全一致，
    // 只是起步更小、空闲连接不再常驻 8 KiB）。
    constexpr std::size_t kReadSpace = 2048;
    net::buffer buf{kReadSpace};

    for (;;) {
        buf.ensure_writable(kReadSpace);
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
    // ---- 自动补 Content-Length ----
    //
    // **这是必须的**：响应既没有 Content-Length、又不是 chunked、也不是 101 升级时，
    // 客户端无从判断 body 在哪里结束，只能一直等连接关闭 —— curl / wrk 会直接挂死到超时。
    //
    // 这个缺陷长期没被发现，原因是**我们自己的压测客户端太宽容**：
    // `mfbench http-load` 只统计"收到了多少字节"，从不校验响应是否完整。
    // 换成真实 HTTP 客户端（curl / wrk）第一次跑就暴露了。
    // 教训：自研压测端只能测吞吐，**不能替代**对协议正确性的第三方校验。
    if (resp.status != 101) {
        bool has_length = false;
        for (const auto& [k, v] : resp.headers) {
            (void)v;
            if (k.size() != 14) { continue; }
            const char* want = "content-length";
            bool same = true;
            for (std::size_t i = 0; i < 14; ++i) {
                const char c = (k[i] >= 'A' && k[i] <= 'Z') ? static_cast<char>(k[i] - 'A' + 'a') : k[i];
                if (c != want[i]) { same = false; break; }
            }
            if (same) { has_length = true; break; }
        }
        if (!has_length) {
            std::size_t len = 0;
            if (resp.stream_file) {
                len = static_cast<std::size_t>(resp.file_length);
            } else if (!resp.body_view.empty()) {
                len = resp.body_view.size();
            } else {
                len = resp.body.size();
            }
            const auto [end, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), len);
            (void)ec;
            head += "Content-Length: ";
            head.append(numbuf, static_cast<std::size_t>(end - numbuf));
            head += "\r\n";
        }
    }

    if (resp.status == 101) {
        head += "Connection: Upgrade\r\n\r\n";  // WebSocket 升级响应
    } else {
        head += "Connection: keep-alive\r\n\r\n";
    }

    if (!co_await write_all(*ctx_, s, head.data(), head.size())) { co_return false; }
    if (head_only) { co_return true; }

    if (resp.stream_file) {
        // 用平台原生句柄，不用 std::ifstream。
        // 实测（本机 Windows/NVMe/页缓存热）：ifstream 1.46 GB/s、ReadFile 5.84~7.16 GB/s，
        // 而且 ifstream 与块大小无关（64 KiB 和 1 MiB 一样慢）—— 它就是大文件发送的瓶颈。
        // 详见 file_io.hpp 里 native_file 的说明。
        native_file in;
        if (!in.open(resp.file_path)) { co_return false; }

        std::vector<char> chunk(kChunk);
        std::uint64_t offset = resp.file_offset;
        std::uint64_t remaining = resp.file_length;

        // ---- 诊断打点（P12 遗留：Windows 侧大文件只有 0.9 GB/s，根因未定位）----
        // 把一次发送拆成"读文件"与"写套接字"，先确定时间花在哪一侧，而不是继续猜假设。
        double t_read = 0.0;
        double t_write = 0.0;
        std::size_t nchunk = 0;
        std::size_t nwrite = 0;
        const auto clock_now = []() noexcept {
            return std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };

        while (remaining > 0) {
            const std::size_t want =
                remaining < kChunk ? static_cast<std::size_t>(remaining) : kChunk;
            const double a = clock_now();
            const std::size_t got = in.read_at(offset, chunk.data(), want);
            const double b = clock_now();
            if (got == 0) { break; }
            if (!co_await write_all(*ctx_, s, chunk.data(), got)) { co_return false; }
            const double c = clock_now();
            t_read += b - a;
            t_write += c - b;
            ++nchunk;
            ++nwrite;
            offset += got;
            remaining -= got;
        }
        if (nchunk > 0) {
            const double tot = t_read + t_write;
            std::fprintf(stderr,
                         "[stream] 块=%zu 写次数=%zu  读=%.3fs(%.1f%%)  写=%.3fs(%.1f%%)  合计=%.3fs\n",
                         nchunk, nwrite, t_read, tot > 0 ? 100.0 * t_read / tot : 0.0, t_write,
                         tot > 0 ? 100.0 * t_write / tot : 0.0, tot);
        }
        co_return true;
    }

    if (!resp.body_view.empty()) {  // 非拥有 body：零拷贝发送
        co_return co_await write_all(*ctx_, s, resp.body_view.data(), resp.body_view.size());
    }
    if (!resp.body.empty()) {
        co_return co_await write_all(*ctx_, s, resp.body.data(), resp.body.size());
    }
    co_return true;
}

}  // namespace mfweb::http
