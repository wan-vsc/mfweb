#include <mfweb/io/io_engine.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mfweb::io {

std::string io_status::message() const {
    if (error == 0) { return "ok"; }
#ifdef _WIN32
    char* text = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&text), 0, nullptr);
    std::string result;
    if (n != 0 && text != nullptr) {
        result.assign(text, n);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
    } else {
        result = "未知错误";
    }
    if (text != nullptr) { LocalFree(text); }
    result += " (code " + std::to_string(error) + ")";
    return result;
#else
    return "code " + std::to_string(error);
#endif
}

}  // namespace mfweb::io
