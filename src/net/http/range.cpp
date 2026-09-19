#include <mfweb/net/http/range.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mfweb::http {
namespace {

std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) { ++b; }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) { --e; }
    return s.substr(b, e - b);
}

[[nodiscard]] bool parse_u64(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty()) { return false; }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

// 解析单个区间；返回 false 表示语法非法，unsat 表示区间不满足
[[nodiscard]] bool parse_spec(std::string_view spec, std::uint64_t size, byte_range& out,
                              bool& unsat) {
    spec = trim(spec);
    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) { return false; }

    const std::string_view a = spec.substr(0, dash);
    const std::string_view b = spec.substr(dash + 1);

    if (a.empty()) {
        // 后缀形式 -N
        std::uint64_t n = 0;
        if (!parse_u64(b, n)) { return false; }
        if (n == 0) {
            unsat = true;
            return true;
        }
        out.first = (n >= size) ? 0 : size - n;
        out.last = (size == 0) ? 0 : size - 1;
        return true;
    }

    std::uint64_t first = 0;
    if (!parse_u64(a, first)) { return false; }

    if (b.empty()) {
        // a- 形式
        if (first >= size) {
            unsat = true;
            return true;
        }
        out.first = first;
        out.last = size - 1;
        return true;
    }

    std::uint64_t last = 0;
    if (!parse_u64(b, last)) { return false; }

    if (first > last || first >= size) {
        unsat = true;
        return true;
    }
    out.first = first;
    out.last = std::min(last, size - 1);
    return true;
}

}  // namespace

bool parse_range(std::string_view header, std::uint64_t file_size, std::vector<byte_range>& out,
                 bool& unsatisfiable) {
    out.clear();
    unsatisfiable = false;
    header = trim(header);

    if (header.empty()) { return true; }  // 无 Range

    // 必须是 "bytes=" 前缀（大小写不敏感）
    if (header.size() < 6) { return false; }
    std::string_view unit = header.substr(0, 6);
    if (!(unit[0] == 'b' || unit[0] == 'B') || !(unit[1] == 'y' || unit[1] == 'Y') ||
        !(unit[2] == 't' || unit[2] == 'T') || !(unit[3] == 'e' || unit[3] == 'E') ||
        !(unit[4] == 's' || unit[4] == 'S') || unit[5] != '=') {
        return true;  // 非 bytes 单位：按无 Range 处理
    }

    if (file_size == 0) {
        unsatisfiable = true;  // 空文件无法满足任何区间
        return true;
    }

    std::string_view rest = header.substr(6);
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t comma = rest.find(',', start);
        const std::string_view spec =
            (comma == std::string_view::npos) ? rest.substr(start) : rest.substr(start, comma - start);

        byte_range r;
        if (!parse_spec(spec, file_size, r, unsatisfiable)) { return false; }
        if (unsatisfiable) { return true; }
        out.push_back(r);

        if (comma == std::string_view::npos) { break; }
        start = comma + 1;
    }
    return true;
}

std::string make_etag(std::uint64_t size, std::time_t mtime) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "\"%llx-%llx\"", static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(mtime));
    return buf;
}

bool etag_matches(std::string_view header, std::string_view etag) {
    if (header.empty()) { return false; }
    if (header == "*") { return true; }
    std::size_t start = 0;
    while (start <= header.size()) {
        const std::size_t comma = header.find(',', start);
        std::string_view tag =
            (comma == std::string_view::npos) ? header.substr(start) : header.substr(start, comma - start);
        if (trim(tag) == etag) { return true; }
        if (comma == std::string_view::npos) { break; }
        start = comma + 1;
    }
    return false;
}

}  // namespace mfweb::http
