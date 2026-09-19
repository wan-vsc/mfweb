#include <mfweb/net/http/http_common.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace mfweb::http {
namespace {

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

method parse_method(std::string_view m) noexcept {
    if (m == "GET") { return method::get; }
    if (m == "HEAD") { return method::head; }
    if (m == "POST") { return method::post; }
    if (m == "PUT") { return method::put; }
    if (m == "DELETE") { return method::delete_; }
    if (m == "OPTIONS") { return method::options; }
    return method::unknown;
}

std::string_view method_name(method m) noexcept {
    switch (m) {
        case method::get: return "GET";
        case method::head: return "HEAD";
        case method::post: return "POST";
        case method::put: return "PUT";
        case method::delete_: return "DELETE";
        case method::options: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

std::string_view status_text(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 416: return "Range Not Satisfiable";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string_view request::header(std::string_view name) const noexcept {
    for (const auto& h : headers) {
        if (iequals(h.name, name)) { return h.value; }
    }
    return {};
}

void request::reset() noexcept {
    method_ = method::unknown;
    target = {};
    minor_version = 1;
    headers.clear();
    body = {};
    keep_alive = true;
    chunked = false;
}

std::string format_http_date(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

std::string now_http_date() { return format_http_date(std::time(nullptr)); }

}  // namespace mfweb::http
