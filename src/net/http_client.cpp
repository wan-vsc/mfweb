#include <mfweb/net/http_client.hpp>

#include <charconv>

namespace mfweb::net {
namespace {

[[nodiscard]] std::error_code make_err(int code) { return std::error_code(code, std::system_category()); }

// 解析响应头：返回 (状态码, 头结束位置)；失败返回 false
[[nodiscard]] bool parse_status_and_headers(std::string_view raw, int& status,
                                            std::vector<std::pair<std::string, std::string>>& headers,
                                            std::size_t& head_end) {
    head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) { return false; }
    head_end += 4;

    const std::string_view head = raw.substr(0, head_end);
    const std::size_t eol = head.find("\r\n");
    const std::string_view status_line = head.substr(0, eol == std::string_view::npos ? head.size() : eol);

    // HTTP/1.1 200 OK
    const std::size_t sp1 = status_line.find(' ');
    if (sp1 == std::string_view::npos) { return false; }
    const std::size_t sp2 = status_line.find(' ', sp1 + 1);
    const std::string_view code_text = status_line.substr(
        sp1 + 1, (sp2 == std::string_view::npos ? status_line.size() : sp2) - sp1 - 1);
    int code = 0;
    const auto [ptr, ec] = std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
    if (ec != std::errc{} || ptr != code_text.data() + code_text.size()) { return false; }
    status = code;

    std::size_t pos = (eol == std::string_view::npos) ? head.size() : eol + 2;
    while (pos < head.size()) {
        const std::size_t nl = head.find("\r\n", pos);
        if (nl == std::string_view::npos || nl == pos) { break; }
        const std::string_view line = head.substr(pos, nl - pos);
        pos = nl + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) { continue; }
        std::string_view name = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) { value.remove_prefix(1); }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) { value.remove_suffix(1); }
        headers.emplace_back(std::string(name), std::string(value));
    }
    return true;
}

[[nodiscard]] std::string_view find_header(const std::vector<std::pair<std::string, std::string>>& hs,
                                           std::string_view name) {
    for (const auto& [k, v] : hs) {
        if (k.size() != name.size()) { continue; }
        bool same = true;
        for (std::size_t i = 0; i < k.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(k[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) { same = false; break; }
        }
        if (same) { return v; }
    }
    return {};
}

// 解码 chunked 响应体（服务端 → 客户端方向不带掩码，与 WebSocket 无关）
[[nodiscard]] bool decode_chunked(std::string_view raw, std::string& out) {
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = raw.find("\r\n", pos);
        if (nl == std::string_view::npos) { return false; }
        std::size_t digits = 0;
        while (digits < nl - pos && raw[pos + digits] != ';') { ++digits; }
        std::size_t size = 0;
        for (std::size_t i = 0; i < digits; ++i) {
            const char c = raw[pos + i];
            int d = -1;
            if (c >= '0' && c <= '9') { d = c - '0'; }
            else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
            else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
            if (d < 0) { return false; }
            size = size * 16 + static_cast<std::size_t>(d);
        }
        pos = nl + 2;
        if (size == 0) { return true; }  // 忽略 trailer
        if (pos + size + 2 > raw.size()) { return false; }
        out.append(raw.data() + pos, size);
        pos += size + 2;  // 跳过数据与结尾 CRLF
    }
}

}  // namespace

coro::task<http_client::outcome> http_client::request(http::method m, std::string target,
                                                      std::string body) {
    const io::native_socket s = make_client_socket(*ctx_);
    if (s == io::k_invalid_socket) { co_return mfweb::err(make_err(1)); }

    const sockaddr_in addr = loopback_address(port_);
    const auto cs = co_await coro::async_connect(*ctx_, s, reinterpret_cast<const sockaddr*>(&addr),
                                                 sizeof(addr));
    if (!cs.ok()) {
        close_socket(s);
        co_return mfweb::err(make_err(cs.error));
    }

    std::string req;
    req += std::string(http::method_name(m)) + " " + target + " HTTP/1.1\r\n";
    req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    req += "User-Agent: mfweb-client/0.1\r\n";
    req += "Accept: */*\r\n";
    req += "Connection: close\r\n";
    if (!body.empty() || m == http::method::post || m == http::method::put) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    const auto ws = co_await coro::async_write(*ctx_, s, req.data(), req.size());
    if (!ws.ok()) {
        close_socket(s);
        co_return mfweb::err(make_err(ws.error));
    }

    std::string raw;
    char chunk[4096];
    bool head_parsed = false;
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t head_end = 0;

    for (;;) {
        const auto rs = co_await coro::async_read(*ctx_, s, chunk, sizeof(chunk));
        if (!rs.ok()) {
            close_socket(s);
            co_return mfweb::err(make_err(rs.error));
        }
        if (rs.bytes == 0) { break; }  // 服务端关闭
        raw.append(chunk, rs.bytes);

        if (!head_parsed) {
            if (!parse_status_and_headers(raw, status, headers, head_end)) { continue; }
            head_parsed = true;
        }

        // 有 Content-Length 时可以提前结束
        const std::string_view cl = find_header(headers, "content-length");
        if (!cl.empty()) {
            std::size_t want = 0;
            const auto [ptr, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), want);
            if (ec == std::errc{} && ptr == cl.data() + cl.size()) {
                if (raw.size() >= head_end + want) { break; }
            }
        }
    }
    close_socket(s);

    if (!head_parsed) { co_return mfweb::err(make_err(2)); }

    http::response resp;
    resp.status = status;
    resp.headers = std::move(headers);

    const std::string_view te = find_header(resp.headers, "transfer-encoding");
    const std::string_view body_view = std::string_view(raw).substr(head_end);
    if (!te.empty() && te.find("chunked") != std::string_view::npos) {
        if (!decode_chunked(body_view, resp.body)) { co_return mfweb::err(make_err(3)); }
    } else {
        const std::string_view cl = find_header(resp.headers, "content-length");
        if (!cl.empty()) {
            std::size_t want = 0;
            const auto [ptr, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), want);
            if (ec == std::errc{} && ptr == cl.data() + cl.size() && body_view.size() >= want) {
                resp.body.assign(body_view.substr(0, want));
            } else {
                resp.body.assign(body_view);
            }
        } else {
            resp.body.assign(body_view);
        }
    }

    co_return mfweb::ok(std::move(resp));
}

coro::task<http_client::outcome> http_client::get(std::string target) {
    return request(http::method::get, std::move(target));
}

async_result<http_client::response_type> http_client::request_async(http::method m,
                                                                   std::string target,
                                                                   std::string body) {
    async_result<response_type> pending;
    // 把协程挂到事件循环上：完成时把结果交给 async_result，从而打通 then 链
    struct runner {
        static coro::task<void> run(runtime::io_context& ctx, http_client& client, http::method m,
                                    std::string target, std::string body,
                                    async_result<response_type> out) {
            auto r = co_await client.request(m, std::move(target), std::move(body));
            if (r) {
                out.set_value(std::move(r.value()));
            } else {
                out.set_error(r.error());
            }
        }
    };
    ctx_->spawn(runner::run(*ctx_, *this, m, std::move(target), std::move(body), pending));
    return pending;
}

async_result<http_client::response_type> http_client::get_async(std::string target) {
    return request_async(http::method::get, std::move(target));
}

}  // namespace mfweb::net
