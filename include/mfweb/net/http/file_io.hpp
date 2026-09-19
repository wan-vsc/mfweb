#pragma once

// mfweb::http —— 文件读写辅助（正确处理 UTF-8 路径）。
//
// 关键背景（本机踩过两次的坑）：
//   1. std::ifstream(char*) 在 Windows 上按 **ANSI 代码页(936/GBK)** 解释路径，
//      而本仓库的交付目录名含中文、磁盘路径是 UTF-8 字节串 → 打开失败。
//   2. _stat 同理。
// 因此统一走：UTF-8 → UTF-16（MultiByteToWideChar / std::filesystem::u8path），
// 再用宽字符 API。Linux 侧直接透传（路径本来就是 UTF-8 字节流）。

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mfweb::http {

// std::filesystem::u8path 在 C++20 已废弃；这里用 path 的 u8string 构造代替。
[[nodiscard]] inline std::filesystem::path u8path(std::string_view s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

[[nodiscard]] inline std::ifstream open_file(const std::string& path) {
    // 用 u8path 把 UTF-8 转成平台原生路径（Windows 下是 UTF-16），
    // 直接 ifstream(path.c_str()) 会用 ANSI 代码页解释中文路径而打开失败。
    return std::ifstream(u8path(path), std::ios::binary);
}

#ifdef _WIN32
[[nodiscard]] inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) { return {}; }
    const int n =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    }
    return w;
}

[[nodiscard]] inline std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) { return {}; }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr,
                                        0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n,
                              nullptr, nullptr);
    }
    return out;
}
#endif

}  // namespace mfweb::http
