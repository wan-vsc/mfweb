#pragma once

// mfweb::reflect —— 枚举反射（编译期扫描 + 名字提取）。
//
// 原理：让编译器在函数签名里打印出枚举值的完整限定名，再在编译期解析。
//   MSVC : __FUNCSIG__          形如 "... enum_name_signature<enum color,color::blue>(void) noexcept"
//   GCC  : __PRETTY_FUNCTION__  形如 "... [with E = color; E value = color::blue]"
// 这是 magic_enum 的成熟做法：纯标准 C++、无第三方依赖、无预处理宏。
//
// **代价必须说清楚**：名字提取靠模板实例化，每个待扫描的整数值都要实例化一次。
// 因此扫描范围是有界的（kEnumScanMin..kEnumScanMax，默认 -32..96 共 128 个值），
// 超出范围的枚举值取不到名字（返回空串）。范围可在编译期用宏调整。
//
// 本机实测：一个 3 值枚举 + 一个 8 值枚举的单元测试，编译时间增加可忽略。

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

#ifndef MFWEB_ENUM_SCAN_MIN
#define MFWEB_ENUM_SCAN_MIN (-32)
#endif
#ifndef MFWEB_ENUM_SCAN_MAX
#define MFWEB_ENUM_SCAN_MAX 96
#endif

namespace mfweb::reflect {

inline constexpr int kEnumScanMin = MFWEB_ENUM_SCAN_MIN;
inline constexpr int kEnumScanMax = MFWEB_ENUM_SCAN_MAX;

namespace detail {

template <class E, E Value>
[[nodiscard]] constexpr std::string_view enum_signature() noexcept {
#ifdef _MSC_VER
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

// 从签名里截出枚举值的名字（不含限定前缀）
template <class E, E Value>
[[nodiscard]] constexpr std::string_view enum_name_raw() noexcept {
    constexpr std::string_view sig = enum_signature<E, Value>();
#ifdef _MSC_VER
    // MSVC 实际签名形如：... sig<enum color,color::red>(void) noexcept
    //                    ... sig<enum ns::inner,ns::inner::y>(void) noexcept
    //                    ... sig<enum color,(enum color)0x63>(void) noexcept  ← 未命名值
    constexpr std::string_view marker = "<enum ";
    const std::size_t start = sig.find(marker);
    if (start == std::string_view::npos) { return {}; }
    const std::size_t comma = sig.find(',', start + marker.size());
    if (comma == std::string_view::npos) { return {}; }
    const std::size_t end = sig.find('>', comma);
    if (end == std::string_view::npos) { return {}; }
    std::string_view full = sig.substr(comma + 1, end - comma - 1);
#else
    // GCC/Clang 的 __PRETTY_FUNCTION__ 形如：
    //   "... enum_signature() [with E = color; E Value = color::blue]"
    // 注意模板形参在源码里声明为 <class E, E Value>，GCC 会**原样回显形参名**，
    // 所以标记是 "Value = "（大写 V），不是 "value = "。
    // —— 这正是 Windows 上 183/183 全绿、Linux 上这 3 个用例挂掉的原因。
    constexpr std::string_view marker = "Value = ";
    constexpr std::string_view marker_alt = "value = ";
    std::size_t start = sig.find(marker);
    if (start != std::string_view::npos) {
        start += marker.size();
    } else {
        start = sig.find(marker_alt);
        if (start == std::string_view::npos) { return {}; }
        start += marker_alt.size();
    }
    const std::size_t begin = start;
    std::size_t end = sig.find(';', begin);
    if (end == std::string_view::npos) { end = sig.find(']', begin); }
    if (end == std::string_view::npos) { return {}; }
    std::string_view full = sig.substr(begin, end - begin);
#endif
    // 去掉 "color::" 这样的限定前缀
    const std::size_t colon = full.rfind("::");
    return (colon == std::string_view::npos) ? full : full.substr(colon + 2);
}

// 有效枚举值：名字非空且不含 '(' ')' ':'（否则说明该值不存在，签名退化成表达式）
template <class E, E Value>
[[nodiscard]] constexpr bool is_named_value() noexcept {
    const std::string_view name = enum_name_raw<E, Value>();
    return !name.empty() && name.find('(') == std::string_view::npos &&
           name.find(')') == std::string_view::npos && name.find(':') == std::string_view::npos &&
           name.find('<') == std::string_view::npos;
}

template <class E, int... I>
[[nodiscard]] constexpr std::string_view enum_name_impl(E value,
                                                        std::integer_sequence<int, I...>) noexcept {
    std::string_view result{};
    // 折叠展开：命中第一个"值相等且名字有效"的候选即取名字
    ((value == static_cast<E>(I) && is_named_value<E, static_cast<E>(I)>()
          ? (result = enum_name_raw<E, static_cast<E>(I)>(), true)
          : false) ||
     ...);
    return result;
}

template <class E, int... I>
[[nodiscard]] constexpr std::size_t collect_values(std::array<E, sizeof...(I)>& out,
                                                   std::integer_sequence<int, I...>) noexcept {
    std::size_t n = 0;
    const auto push = [&out, &n](E v) constexpr {
        for (std::size_t k = 0; k < n; ++k) {
            if (out[k] == v) { return; }
        }
        out[n++] = v;
    };
    // 必须用**逗号折叠**：|| 折叠会短路，只会收集到第一个有名字的值
    ((is_named_value<E, static_cast<E>(I)>() ? static_cast<void>(push(static_cast<E>(I)))
                                             : static_cast<void>(0)),
     ...);
    return n;
}

// 把 [0, N) 位移成 [Min, Min+N)
template <int Min, int... I>
[[nodiscard]] constexpr auto offset_sequence(std::integer_sequence<int, I...>) noexcept {
    return std::integer_sequence<int, (Min + I)...>{};
}

}  // namespace detail

inline constexpr std::size_t kEnumScanCount =
    static_cast<std::size_t>(kEnumScanMax - kEnumScanMin);

// 待扫描的整数值序列：kEnumScanMin .. kEnumScanMax-1
inline constexpr auto kEnumScanSequence =
    detail::offset_sequence<kEnumScanMin>(std::make_integer_sequence<int, kEnumScanCount>{});

// 枚举值 → 名字；取不到返回空串
template <class E>
[[nodiscard]] constexpr std::string_view enum_name(E value) noexcept {
    static_assert(std::is_enum_v<E>, "enum_name 要求枚举类型");
    return detail::enum_name_impl<E>(value, kEnumScanSequence);
}

// 全部"有名字"的枚举值（去重后），返回 (数组, 个数)
template <class E>
[[nodiscard]] constexpr auto enum_values() noexcept {
    static_assert(std::is_enum_v<E>, "enum_values 要求枚举类型");
    std::array<E, kEnumScanCount> out{};
    const std::size_t n = detail::collect_values<E>(out, kEnumScanSequence);
    return std::pair{out, n};
}

}  // namespace mfweb::reflect
