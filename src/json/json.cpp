#include <mfweb/json/json.hpp>

#include <mfweb/reflect/enum.hpp>
#include <mfweb/reflect/macro.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <type_traits>

namespace mfweb::json {
namespace {

constexpr std::string_view kEscapes = "\"\\/\b\f\n\r\t";

void append_escaped(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch) & 0xFF);
                    out += buf;
                } else {
                    out.push_back(ch);  // UTF-8 字节原样透传
                }
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double d) {
    if (std::isnan(d) || std::isinf(d)) {
        out += "null";  // JSON 无 NaN/Inf 表示
        return;
    }
    char buf[32]{};
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
    if (ec == std::errc{}) {
        out.append(buf, ptr);
    } else {
        out += "0";
    }
}

// ---------------------------------------------------------------- 解析器
class parser {
public:
    parser(std::string_view text, std::size_t max_depth) noexcept
        : text_(text), max_depth_(max_depth) {}

    [[nodiscard]] result<value, error> run() {
        skip_ws();
        value v;
        if (!parse_value(v, 0)) { return mfweb::err(err_); }
        skip_ws();
        if (pos_ != text_.size()) {
            fail("结尾有多余内容");
            return mfweb::err(err_);
        }
        return mfweb::ok(std::move(v));
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }

    void bump() noexcept {
        if (at_end()) { return; }
        if (text_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    void skip_ws() noexcept {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { bump(); } else { break; }
        }
    }

    void fail(std::string message) {
        if (err_.message.empty()) {
            err_.line = line_;
            err_.column = col_;
            err_.message = std::move(message);
        }
    }

    [[nodiscard]] bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) { return false; }
        for (std::size_t i = 0; i < word.size(); ++i) { bump(); }
        return true;
    }

    [[nodiscard]] bool parse_value(value& out, std::size_t depth) {
        if (depth > max_depth_) {
            fail("嵌套过深");
            return false;
        }
        if (at_end()) {
            fail("内容意外结束");
            return false;
        }
        switch (peek()) {
            case 'n':
                if (!literal("null")) { fail("非法字面量"); return false; }
                out = value{};
                return true;
            case 't':
                if (!literal("true")) { fail("非法字面量"); return false; }
                out = value{true};
                return true;
            case 'f':
                if (!literal("false")) { fail("非法字面量"); return false; }
                out = value{false};
                return true;
            case '"': {
                std::string s;
                if (!parse_string(s)) { return false; }
                out = value{std::move(s)};
                return true;
            }
            case '[': return parse_array(out, depth);
            case '{': return parse_object(out, depth);
            default: return parse_number(out);
        }
    }

    [[nodiscard]] bool parse_number(value& out) {
        const std::size_t start = pos_;
        if (peek() == '-') { bump(); }
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            fail("非法数字");
            return false;
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) { bump(); }
        if (peek() == '.') {
            bump();
            if (!std::isdigit(static_cast<unsigned char>(peek()))) { fail("小数点后缺数字"); return false; }
            while (std::isdigit(static_cast<unsigned char>(peek()))) { bump(); }
        }
        if (peek() == 'e' || peek() == 'E') {
            bump();
            if (peek() == '+' || peek() == '-') { bump(); }
            if (!std::isdigit(static_cast<unsigned char>(peek()))) { fail("指数缺数字"); return false; }
            while (std::isdigit(static_cast<unsigned char>(peek()))) { bump(); }
        }
        double d = 0.0;
        const auto* begin = text_.data() + start;
        const auto [ptr, ec] = std::from_chars(begin, text_.data() + pos_, d);
        if (ec != std::errc{} || ptr != text_.data() + pos_) {
            fail("数字解析失败");
            return false;
        }
        out = value{d};
        return true;
    }

    [[nodiscard]] static int hex_digit(char c) noexcept {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    }

    void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    [[nodiscard]] bool parse_hex4(unsigned& out) {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) { fail("\\u 转义不完整"); return false; }
            const int d = hex_digit(peek());
            if (d < 0) { fail("\\u 转义含非法十六进制"); return false; }
            v = v * 16 + static_cast<unsigned>(d);
            bump();
        }
        out = v;
        return true;
    }

    [[nodiscard]] bool parse_string(std::string& out) {
        if (peek() != '"') { fail("期望字符串"); return false; }
        bump();
        for (;;) {
            if (at_end()) { fail("字符串未闭合"); return false; }
            const char c = peek();
            if (c == '"') {
                bump();
                return true;
            }
            if (c == '\\') {
                bump();
                if (at_end()) { fail("转义未完成"); return false; }
                const char e = peek();
                bump();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parse_hex4(cp)) { return false; }
                        // 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
                            const std::size_t save_pos = pos_;
                            const std::size_t save_line = line_;
                            const std::size_t save_col = col_;
                            bump();
                            bump();
                            unsigned low = 0;
                            if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save_pos;
                                line_ = save_line;
                                col_ = save_col;
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail("非法转义"); return false;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) { fail("字符串含控制字符"); return false; }
            out.push_back(c);
            bump();
        }
    }

    [[nodiscard]] bool parse_array(value& out, std::size_t depth) {
        bump();  // '['
        value::array_t items;
        skip_ws();
        if (peek() == ']') {
            bump();
            out = value{std::move(items)};
            return true;
        }
        for (;;) {
            value item;
            skip_ws();
            if (!parse_value(item, depth + 1)) { return false; }
            items.push_back(std::move(item));
            skip_ws();
            if (peek() == ',') {
                bump();
                continue;
            }
            if (peek() == ']') {
                bump();
                break;
            }
            fail("数组元素后应为 ',' 或 ']'");
            return false;
        }
        out = value{std::move(items)};
        return true;
    }

    [[nodiscard]] bool parse_object(value& out, std::size_t depth) {
        bump();  // '{'
        value::object_t members;
        skip_ws();
        if (peek() == '}') {
            bump();
            out = value{std::move(members)};
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) { return false; }
            skip_ws();
            if (peek() != ':') {
                fail("键后应为 ':'");
                return false;
            }
            bump();
            value item;
            skip_ws();
            if (!parse_value(item, depth + 1)) { return false; }
            members.emplace_back(std::move(key), std::move(item));
            skip_ws();
            if (peek() == ',') {
                bump();
                continue;
            }
            if (peek() == '}') {
                bump();
                break;
            }
            fail("对象成员后应为 ',' 或 '}'");
            return false;
        }
        out = value{std::move(members)};
        return true;
    }

    std::string_view text_;
    std::size_t max_depth_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;
    error err_{};
};

}  // namespace

// 供 codec.hpp（反射驱动层）复用的两个底层写入原语
namespace detail {

void append_number_json(std::string& out, double d) { append_number(out, d); }

void append_escaped_json(std::string& out, std::string_view s) { append_escaped(out, s); }

}  // namespace detail

result<value, error> parse(std::string_view text, std::size_t max_depth) noexcept {
    parser p{text, max_depth};
    return p.run();
}

void detail::write_value(std::string& out, const value& v) {
    switch (v.type()) {
        case value::kind::null: out += "null"; break;
        case value::kind::boolean: out += v.as_bool() ? "true" : "false"; break;
        case value::kind::number: append_number(out, v.as_number()); break;
        case value::kind::string: append_escaped(out, v.as_string()); break;
        case value::kind::array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : v.as_array()) {
                if (!first) { out.push_back(','); }
                first = false;
                detail::write_value(out, item);
            }
            out.push_back(']');
            break;
        }
        case value::kind::object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, item] : v.as_object()) {
                if (!first) { out.push_back(','); }
                first = false;
                append_escaped(out, k);
                out.push_back(':');
                detail::write_value(out, item);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string serialize(const value& v) {
    std::string out;
    detail::write_value(out, v);
    return out;
}

}  // namespace mfweb::json
