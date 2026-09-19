#pragma once

// mfweb::version —— 框架版本信息（P0 骨架期的第一个真实符号）

#include <string_view>

namespace mfweb {

struct version_info {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string_view stage;
    std::string_view compiler;
};

[[nodiscard]] version_info version() noexcept;

[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace mfweb
