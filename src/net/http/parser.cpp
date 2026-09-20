#include <mfweb/net/http/parser.hpp>

#include <cstdlib>

namespace mfweb::http {
namespace {

// 跳过行首空白（OWS）
std::string_view trim_left(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) { ++i; }
    return s.substr(i);
}

std::string_view trim_right(std::string_view s) noexcept {
    std::size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) { n--; }
    return s.substr(0, n);
}

// 两侧 OWS 都去掉（RFC 7230：字段值两侧的空白无意义）
std::string_view trim_ows(std::string_view s) noexcept {
    return trim_right(trim_left(s));
}

}  // namespace

request_parser::request_parser(std::size_t max_head, std::size_t max_body) noexcept
    : max_head_(max_head), max_body_(max_body) {}

void request_parser::reset() noexcept {
    head_.clear();
    body_.clear();
    chunk_raw_.clear();
    content_length_ = 0;
    chunk_remaining_ = 0;
    mode_ = body_mode::none;
    chunk_state_ = chunk_state::size;
    head_done_ = false;
    error_ = "ok";
}

request_parser::result request_parser::feed(const char* data, std::size_t len, request& out,
                                            std::size_t& consumed) {
    consumed = 0;

    if (!head_done_) {
        // 累积头部，同时找空行标记
        head_.append(data, len);
        consumed = len;

        const std::size_t mark = head_.find("\r\n\r\n");
        if (mark == std::string::npos) {
            if (head_.size() > max_head_) {
                error_ = "header too large";
                return result::error;
            }
            return result::need_more;
        }

        // 头已完整：解析请求行与头字段
        head_done_ = true;
        const std::size_t head_bytes = mark + 4;
        const std::size_t leftover = head_.size() - head_bytes;

        if (!parse_head(out)) { return result::error; }

        if (out.chunked) {
            mode_ = body_mode::chunked;
        } else if (content_length_ > 0) {
            mode_ = body_mode::length;
        } else {
            // 无 body：请求完整。若空行后还粘着下一请求的字节（pipelining），
            // 只消费头部部分，把多余字节留给调用方。
            consumed = leftover > 0 ? head_bytes : head_.size();
            out.body = {};
            return result::complete;
        }

        // 把空行后已随头部一起读入的字节挪进对应缓冲
        if (leftover > 0) {
            if (mode_ == body_mode::chunked) {
                chunk_raw_.append(head_.data() + head_bytes, leftover);
            } else {
                body_.append(head_.data() + head_bytes, leftover);
            }
        }
        consumed = head_.size();
    } else {
        if (mode_ == body_mode::chunked) {
            chunk_raw_.append(data, len);
        } else {
            body_.append(data, len);
        }
        consumed = len;
    }

    if (mode_ == body_mode::chunked) { return feed_chunked(out); }

    if (body_.size() > max_body_) {
        error_ = "body too large";
        return result::error;
    }
    if (body_.size() >= content_length_) {
        out.body = std::string_view(body_).substr(0, content_length_);
        return result::complete;
    }
    return result::need_more;
}

// chunked 解码（RFC 7230 §4.1）：
//     chunk = chunk-size [;ext] CRLF chunk-data CRLF
//     结束 = "0" CRLF [trailer-part] CRLF
// trailer 字段一律忽略（本项目不用它们）。
request_parser::result request_parser::feed_chunked(request& out) {
    for (;;) {
        switch (chunk_state_) {
            case chunk_state::size: {
                const std::size_t nl = chunk_raw_.find("\r\n");
                if (nl == std::string::npos) {
                    if (chunk_raw_.size() > 4096) {
                        error_ = "chunk-size line too long";
                        return result::error;
                    }
                    return result::need_more;
                }
                std::size_t digits = 0;
                while (digits < nl && chunk_raw_[digits] != ';') { ++digits; }
                if (digits == 0) {
                    error_ = "empty chunk size";
                    return result::error;
                }
                std::size_t value = 0;
                for (std::size_t i = 0; i < digits; ++i) {
                    const char ch = chunk_raw_[i];
                    int digit = -1;
                    if (ch >= '0' && ch <= '9') { digit = ch - '0'; }
                    else if (ch >= 'a' && ch <= 'f') { digit = ch - 'a' + 10; }
                    else if (ch >= 'A' && ch <= 'F') { digit = ch - 'A' + 10; }
                    if (digit < 0) {
                        error_ = "invalid chunk size";
                        return result::error;
                    }
                    value = value * 16 + static_cast<std::size_t>(digit);
                    if (value > max_body_) {
                        error_ = "chunk too large";
                        return result::error;
                    }
                }
                chunk_raw_.erase(0, nl + 2);
                if (value == 0) {
                    chunk_state_ = chunk_state::trailer;
                } else {
                    chunk_remaining_ = value;
                    chunk_state_ = chunk_state::data;
                }
                break;
            }

            case chunk_state::data: {
                if (chunk_raw_.empty()) { return result::need_more; }
                const std::size_t take = chunk_raw_.size() < chunk_remaining_
                                             ? chunk_raw_.size()
                                             : chunk_remaining_;
                body_.append(chunk_raw_.data(), take);
                chunk_raw_.erase(0, take);
                chunk_remaining_ -= take;
                if (body_.size() > max_body_) {
                    error_ = "body too large";
                    return result::error;
                }
                if (chunk_remaining_ == 0) {
                    chunk_state_ = chunk_state::data_crlf;
                } else {
                    return result::need_more;
                }
                break;
            }

            case chunk_state::data_crlf: {
                if (chunk_raw_.size() < 2) { return result::need_more; }
                if (chunk_raw_[0] != '\r' || chunk_raw_[1] != '\n') {
                    error_ = "missing CRLF after chunk data";
                    return result::error;
                }
                chunk_raw_.erase(0, 2);
                chunk_state_ = chunk_state::size;
                break;
            }

            case chunk_state::trailer: {
                const std::size_t nl = chunk_raw_.find("\r\n");
                if (nl == std::string::npos) {
                    if (chunk_raw_.size() > 8192) {
                        error_ = "trailer too long";
                        return result::error;
                    }
                    return result::need_more;
                }
                if (nl == 0) {
                    chunk_raw_.erase(0, 2);
                    chunk_state_ = chunk_state::done;
                    out.body = std::string_view(body_);
                    return result::complete;
                }
                chunk_raw_.erase(0, nl + 2);  // 忽略 trailer 字段
                break;
            }

            case chunk_state::done:
                out.body = std::string_view(body_);
                return result::complete;
        }
    }
}

bool request_parser::parse_head(request& out) {
    out.reset();

    // 1) 请求行
    const std::size_t line_end = head_.find("\r\n");
    if (line_end == std::string::npos) {
        error_ = "missing CRLF after request line";
        return false;
    }
    const std::string_view request_line(head_.data(), line_end);

    // METHOD SP TARGET SP VERSION
    const std::size_t sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) {
        error_ = "malformed request line";
        return false;
    }
    const std::size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        error_ = "malformed request line";
        return false;
    }

    out.method_ = parse_method(request_line.substr(0, sp1));
    out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = trim_left(request_line.substr(sp2 + 1));

    if (version == "HTTP/1.1") {
        out.minor_version = 1;
    } else if (version == "HTTP/1.0") {
        out.minor_version = 0;
    } else {
        error_ = "unsupported HTTP version";
        return false;
    }

    // 2) 头字段
    std::size_t pos = line_end + 2;
    while (pos < head_.size()) {
        const std::size_t nl = head_.find("\r\n", pos);
        if (nl == std::string::npos) {
            error_ = "unterminated header";
            return false;
        }
        const std::string_view line(head_.data() + pos, nl - pos);
        pos = nl + 2;
        if (line.empty()) { break; }  // 空行：头部结束（防御，正常应已被标记截断）

        header_view h;
        if (!parse_header_line(line, h)) {
            error_ = "malformed header line";
            return false;
        }
        out.headers.push_back(h);
    }

    // 3) 语义
    const std::string_view cl = out.header("content-length");
    if (!cl.empty()) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(cl.data(), &end, 10);
        if (end == cl.data() || v > max_body_) {
            error_ = "invalid content-length";
            return false;
        }
        content_length_ = static_cast<std::size_t>(v);
    }
    if (out.header("transfer-encoding").find("chunked") != std::string_view::npos) {
        out.chunked = true;
    }

    // keep-alive
    const std::string_view conn = out.header("connection");
    if (out.minor_version == 1) {
        out.keep_alive = conn.find("close") == std::string_view::npos;
    } else {
        out.keep_alive = conn.find("keep-alive") != std::string_view::npos;
    }

    return true;
}

bool request_parser::parse_header_line(std::string_view line, header_view& out) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) { return false; }
    out.name = trim_right(line.substr(0, colon));
    out.value = trim_ows(line.substr(colon + 1));  // 两侧 OWS 都去掉
    if (out.name.empty()) { return false; }
    return true;
}

}  // namespace mfweb::http
