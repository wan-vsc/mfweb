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
#ifndef NOMINMAX
// **必须定义 NOMINMAX**：否则 <windows.h> 会把 min/max 定义成宏，
// 破坏本库（以及任何使用者代码）里的 std::max / numeric_limits<T>::max() / time_point::max()。
// 项目自身的 CMake 构建在 cmake/CompilerOptions.cmake 里定义了它，
// 但**把 mfweb 作为库引入自己工程的使用者不会** —— 所以在头文件里兜住。
// 这个疏漏是照 README 的快速上手示例、用裸 cl 命令编译时暴露的。
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
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

// ---------------------------------------------------------------- 原生文件句柄
//
// **为什么不用 std::ifstream 读文件**：实测（本机 Windows / NVMe / 页缓存热）——
//
//     块大小      std::ifstream      Win32 ReadFile
//     64 KiB      1.46 GB/s          5.84 GB/s
//     512 KiB     1.47 GB/s          7.16 GB/s
//     1 MiB       1.45 GB/s          6.96 GB/s
//
// **MSVC 的 ifstream 卡在 ~1.46 GB/s，而且与块大小无关** —— 它多了一层流缓冲，
// 开销在小块上也下不来。差了 4~5 倍，而且这正好卡在大文件发送路径上：
// 框架传 2 GiB 只有 0.92 GB/s，而同一台机器裸 loopback TCP 上限是 3.67 GB/s。
//
// 换成平台原生句柄 + 定位读之后，读侧不再是瓶颈。
// 定位读（pread / OVERLAPPED.Offset 同步用法）天然支持 Range 与多线程复用同一句柄。
#ifdef _WIN32
using native_handle = HANDLE;
// 注意：**不能用 constexpr** —— INVALID_HANDLE_VALUE 是 ((HANDLE)(LONG_PTR)-1)，
// 即"把整数强转成指针"，MSVC 会报 C2131（表达式不是常数）。
inline const native_handle k_bad_handle = INVALID_HANDLE_VALUE;
#else
using native_handle = int;
inline constexpr native_handle k_bad_handle = -1;
#endif

class native_file {
public:
    native_file() noexcept = default;
    explicit native_file(const std::string& path) noexcept { (void)open(path); }
    ~native_file() { close(); }

    native_file(const native_file&) = delete;
    native_file& operator=(const native_file&) = delete;
    native_file(native_file&& o) noexcept : h_(o.h_) { o.h_ = k_bad_handle; }
    native_file& operator=(native_file&& o) noexcept {
        if (this != &o) { close(); h_ = o.h_; o.h_ = k_bad_handle; }
        return *this;
    }

    [[nodiscard]] bool open(const std::string& path) noexcept {
        close();
#ifdef _WIN32
        // 路径同样要 UTF-8 -> UTF-16（见本文件开头说明的坑）
        h_ = ::CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return h_ != k_bad_handle;
#else
        h_ = ::open(path.c_str(), O_RDONLY);
        return h_ >= 0;
#endif
    }

    [[nodiscard]] bool is_open() const noexcept { return h_ != k_bad_handle; }

    void close() noexcept {
        if (h_ == k_bad_handle) { return; }
#ifdef _WIN32
        ::CloseHandle(h_);
#else
        ::close(h_);
#endif
        h_ = k_bad_handle;
    }

    // 从 offset 处读最多 n 字节；返回实际读到的字节数（0 表示 EOF 或出错）
    [[nodiscard]] std::size_t read_at(std::uint64_t offset, void* dst, std::size_t n) noexcept {
        if (h_ == k_bad_handle || n == 0) { return 0; }
#ifdef _WIN32
        // 同步句柄 + OVERLAPPED 的 Offset 字段 = 定位读，不移动文件指针
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (::ReadFile(h_, dst, static_cast<DWORD>(n), &got, &ov) == 0) { return 0; }
        return static_cast<std::size_t>(got);
#else
        const ssize_t got = ::pread(h_, dst, n, static_cast<off_t>(offset));
        return got > 0 ? static_cast<std::size_t>(got) : 0;
#endif
    }

private:
    native_handle h_ = k_bad_handle;
};

}  // namespace mfweb::http
