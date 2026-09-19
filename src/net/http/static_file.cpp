#include <mfweb/net/http/static_file.hpp>

#include <mfweb/net/http/file_io.hpp>
#include <mfweb/net/http/range.hpp>

#include <algorithm>
#include <cstdio>
#include <sys/stat.h>

namespace mfweb::http {
namespace {

constexpr std::size_t kChunkSize = 64 * 1024;
constexpr std::uint64_t kMaxMultipart = 8ull * 1024 * 1024;  // 多区间响应在内存里拼，设上限

}  // namespace

bool stat_file(const std::string& path, file_info& out) noexcept {
#ifdef _WIN32
    // 用宽字符版，避免 UTF-8 中文路径被 ANSI 代码页误读
    struct _stat64 st {};
    if (_wstat64(utf8_to_wide(path).c_str(), &st) != 0) { return false; }
    if ((st.st_mode & _S_IFDIR) != 0) { return false; }
#else
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) { return false; }
    if (S_ISDIR(st.st_mode)) { return false; }
#endif
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime = static_cast<std::time_t>(st.st_mtime);
    return true;
}

bool resolve_path(std::string_view root, std::string_view url_path, std::string& out) {
    // 剥离查询串
    const std::size_t q = url_path.find('?');
    if (q != std::string_view::npos) { url_path = url_path.substr(0, q); }

    // URL 解码与 .. 检查
    std::string decoded;
    decoded.reserve(url_path.size());
    for (std::size_t i = 0; i < url_path.size(); ++i) {
        const char c = url_path[i];
        if (c == '%' && i + 2 < url_path.size()) {
            const auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') { return h - '0'; }
                if (h >= 'a' && h <= 'f') { return h - 'a' + 10; }
                if (h >= 'A' && h <= 'F') { return h - 'A' + 10; }
                return -1;
            };
            const int hi = hex(url_path[i + 1]);
            const int lo = hex(url_path[i + 2]);
            if (hi < 0 || lo < 0) { return false; }
            decoded.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            decoded.push_back(c);
        }
    }

    // 目录索引
    if (decoded.empty() || decoded.back() == '/') { decoded += "index.html"; }

    // 逐分量拼接，拒绝任何越界分量。
    // 刻意不用 std::filesystem：它在 Windows 下 value_type 是 wchar_t，与 UTF-8 字节串
    // 混用会踩编码坑；这里用纯字符串逻辑反而更简单、更可控。
    std::string normalized;
    if (!root.empty()) {
        normalized.assign(root.data(), root.size());
        if (normalized.back() != '/') { normalized.push_back('/'); }
    }

    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = decoded.find('/', start);
        const std::string_view seg =
            (slash == std::string_view::npos)
                ? std::string_view(decoded).substr(start)
                : std::string_view(decoded).substr(start, slash - start);

        if (seg == "..") { return false; }  // 拒绝目录穿越
        if (!seg.empty() && seg != ".") {
            normalized.append(seg.data(), seg.size());
            normalized.push_back('/');
        }
        if (slash == std::string_view::npos) { break; }
        start = slash + 1;
    }
    if (normalized.size() > 1 && normalized.back() == '/') { normalized.pop_back(); }
    out = std::move(normalized);
    return true;
}

response build_static_response(const request& req, const std::string& path,
                               const file_info& info, bool head_only) {
    response resp;

    const std::string etag = make_etag(info.size, info.mtime);
    const std::string last_modified = format_http_date(info.mtime);
    resp.set("ETag", etag);
    resp.set("Last-Modified", last_modified);
    resp.set("Accept-Ranges", "bytes");

    // 条件请求：If-None-Match 优先，其次 If-Modified-Since
    const std::string_view inm = req.header("if-none-match");
    bool not_modified = false;
    if (!inm.empty()) {
        not_modified = etag_matches(inm, etag);
    } else {
        // If-Modified-Since 用字符串比较近似处理（同一 GMT 格式）
        const std::string_view ims = req.header("if-modified-since");
        if (!ims.empty() && ims == last_modified) { not_modified = true; }
    }

    if (not_modified) {
        resp.status = 304;
        resp.set("ETag", etag);
        return resp;
    }

    // Range / If-Range
    const std::string_view range_header = req.header("range");
    const std::string_view if_range = req.header("if-range");

    // If-Range 不匹配 → 回全量 200
    bool want_range = !range_header.empty();
    if (want_range && !if_range.empty() && !etag_matches(if_range, etag)) {
        want_range = false;
    }

    if (!want_range) {
        resp.status = 200;
        resp.set("Content-Length", std::to_string(info.size));
        if (!head_only) {
            resp.stream_file = true;
            resp.file_path = path;
            resp.file_offset = 0;
            resp.file_length = info.size;
        }
        return resp;
    }

    // 解析 Range
    std::vector<byte_range> ranges;
    bool unsatisfiable = false;
    if (!parse_range(range_header, info.size, ranges, unsatisfiable)) {
        resp.status = 400;
        resp.body = "Bad Range header";
        return resp;
    }
    if (unsatisfiable) {
        resp.status = 416;
        resp.set("Content-Range", "bytes */" + std::to_string(info.size));
        return resp;
    }

    if (ranges.size() == 1) {
        const byte_range& r = ranges[0];
        resp.status = 206;
        resp.set("Content-Range", "bytes " + std::to_string(r.first) + "-" +
                                      std::to_string(r.last) + "/" + std::to_string(info.size));
        resp.set("Content-Length", std::to_string(r.length()));
        if (!head_only) {
            resp.stream_file = true;
            resp.file_path = path;
            resp.file_offset = r.first;
            resp.file_length = r.length();
        }
        return resp;
    }

    // 多区间 → multipart/byteranges
    resp.status = 206;
    const std::string boundary = "mfweb" +
                                    std::to_string(static_cast<unsigned long long>(info.mtime)) +
                                    std::to_string(info.size);
    resp.set("Content-Type", "multipart/byteranges; boundary=" + boundary);

    if (head_only) { return resp; }

    // 逐段拼进内存（设上限，超限降级为单区间首段 —— 多区间大文件罕见）
    std::uint64_t total = 0;
    for (const auto& r : ranges) { total += r.length(); }
    if (total > kMaxMultipart) {
        // 降级：只回第一个区间
        const byte_range& r = ranges[0];
        resp.headers.clear();
        resp = response{};
        resp.status = 206;
        resp.set("ETag", etag);
        resp.set("Accept-Ranges", "bytes");
        resp.set("Content-Range", "bytes " + std::to_string(r.first) + "-" +
                                      std::to_string(r.last) + "/" + std::to_string(info.size));
        resp.set("Content-Length", std::to_string(r.length()));
        resp.stream_file = true;
        resp.file_path = path;
        resp.file_offset = r.first;
        resp.file_length = r.length();
        return resp;
    }

    // 用 open_file（u8path 正确处理 UTF-8 路径）+ ifstream 的 64 位偏移，
    // 而非 fopen/fseek —— fseek 的 long 在 Windows 是 32 位，会让 >2GB 的文件定位失败。
    std::ifstream in = open_file(path);
    if (!in) {
        resp = response{};
        resp.status = 404;
        return resp;
    }

    std::string body;
    body.reserve(static_cast<std::size_t>(total));
    for (const auto& r : ranges) {
        body += "--" + boundary + "\r\n";
        body += "Content-Type: application/octet-stream\r\n";
        body += "Content-Range: bytes " + std::to_string(r.first) + "-" + std::to_string(r.last) +
                "/" + std::to_string(info.size) + "\r\n\r\n";
        in.seekg(static_cast<std::streamoff>(r.first));
        char buf[kChunkSize];
        std::uint64_t remaining = r.length();
        while (remaining > 0) {
            const std::size_t n = remaining < kChunkSize ? static_cast<std::size_t>(remaining)
                                                         : kChunkSize;
            in.read(buf, static_cast<std::streamsize>(n));
            const std::size_t got = static_cast<std::size_t>(in.gcount());
            if (got == 0) { break; }
            body.append(buf, got);
            remaining -= got;
        }
        body += "\r\n";
    }
    body += "--" + boundary + "--\r\n";

    resp.set("Content-Length", std::to_string(body.size()));
    resp.body = std::move(body);
    return resp;
}

}  // namespace mfweb::http
