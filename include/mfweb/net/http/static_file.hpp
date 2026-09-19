#pragma once

// mfweb::http —— 静态文件服务（含 Range / 条件请求 / 断点续传）。

#include <mfweb/net/http/http_common.hpp>
#include <mfweb/net/http/response.hpp>

#include <cstdint>
#include <ctime>
#include <string>

namespace mfweb::http {

struct file_info {
    std::uint64_t size = 0;
    std::time_t mtime = 0;
};

// 检查文件是否可读并取元数据
[[nodiscard]] bool stat_file(const std::string& path, file_info& out) noexcept;

// 把 URL 路径解析到磁盘文件（防目录穿越）。
// 返回 false 表示路径非法（包含 .. 或逃出根目录）。
[[nodiscard]] bool resolve_path(std::string_view root, std::string_view url_path,
                                std::string& out);

// 组装静态文件响应：处理 Range、If-Range、If-None-Match、If-Modified-Since。
// head_only 用于 HEAD 请求（不发送 body）。
[[nodiscard]] response build_static_response(const request& req, const std::string& path,
                                             const file_info& info, bool head_only);

}  // namespace mfweb::http
