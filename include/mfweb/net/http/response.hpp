#pragma once

// mfweb::http::response —— 响应对象。
//
// body 有两种携带方式：
//   * body 非空：内存 body；
//   * stream_file 为真：从 file_path 的 [file_offset, file_offset+file_length) 流式读入发送，
//     用于静态文件与 Range（避免把大文件整体读进内存）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mfweb::http {

struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string body;                      // 内存 body
    std::string file_path;                 // 流式文件 body
    std::uint64_t file_offset = 0;
    std::uint64_t file_length = 0;
    bool stream_file = false;

    void set(std::string name, std::string value) {
        headers.emplace_back(std::move(name), std::move(value));
    }
};

// 最简错误响应
[[nodiscard]] inline response make_error_response(int status) {
    response r;
    r.status = status;
    r.body = std::string(status_text(status));
    return r;
}

}  // namespace mfweb::http
