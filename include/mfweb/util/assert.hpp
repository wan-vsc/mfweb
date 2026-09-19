#pragma once

// mfweb 断言。与 <cassert> 的区别：
//   1. Release 下同样生效（框架内部不变量被破坏时必须立刻暴露，而不是静默继续跑）；
//   2. 输出文件与行号，便于在百万并发场景下定位。
// 热路径上如需可关闭的检查，请用 MFWEB_DEBUG_ASSERT。

#include <cstdio>
#include <cstdlib>

namespace mfweb::detail {

[[noreturn]] inline void assert_fail(const char* expr, const char* file, int line) noexcept {
    std::fprintf(stderr, "\nmfweb 断言失败: %s\n  位置: %s:%d\n", expr, file, line);
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void assert_fail_msg(const char* expr, const char* msg, const char* file,
                                         int line) noexcept {
    std::fprintf(stderr, "\nmfweb 断言失败: %s\n  说明: %s\n  位置: %s:%d\n", expr, msg, file, line);
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void panic(const char* msg, const char* file, int line) noexcept {
    std::fprintf(stderr, "\nmfweb 致命错误: %s\n  位置: %s:%d\n", msg, file, line);
    std::fflush(stderr);
    std::abort();
}

}  // namespace mfweb::detail

#define MFWEB_ASSERT(expr)     ((expr) ? static_cast<void>(0) : ::mfweb::detail::assert_fail(#expr, __FILE__, __LINE__))

#define MFWEB_ASSERT_MSG(expr, msg)                                              \
    ((expr) ? static_cast<void>(0)                                               \
            : ::mfweb::detail::assert_fail_msg(#expr, (msg), __FILE__, __LINE__))

#define MFWEB_PANIC(msg) ::mfweb::detail::panic((msg), __FILE__, __LINE__)

#ifdef NDEBUG
#define MFWEB_DEBUG_ASSERT(expr) static_cast<void>(0)
#else
#define MFWEB_DEBUG_ASSERT(expr) MFWEB_ASSERT(expr)
#endif
