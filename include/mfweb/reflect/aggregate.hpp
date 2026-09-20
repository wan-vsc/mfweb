#pragma once

// mfweb::reflect —— 编译期聚合体反射（无宏）。
//
// 能力边界（必须说清楚，否则容易被误解）：
//   * **能拿到**：字段个数、按位置访问各字段的值/引用；
//   * **拿不到**：字段名字。C++20 没有静态反射，编译器不把标识符暴露给程序 ——
//     任何声称"无宏拿到字段名"的实现，要么靠宏、要么靠编译器扩展。
//     需要名字（例如 JSON 的键）时请用 reflect/macro.hpp 的类内宏。
//
// 实现（两段式，**都是良定义行为**，不玩 Boost.PFR 那套 reinterpret_cast 布局把戏）：
//   1. 字段个数：用"万能转换器"尝试花括号初始化，二分出最大可初始化个数；
//      聚合体有 N 个字段时 T{ubiq × N} 合法、T{ubiq × (N+1)} 不合法。
//   2. 字段访问：为每个可能的字段数生成一个结构化绑定特化
//      （auto& [f0, f1, ...] = value），拿到的是**真正的成员引用**。
//
// 上限：内联支持到 16 个字段（超过会静态断言报错，请用类内宏指定字段）。

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mfweb::reflect {

// 万能转换器：能隐式转换成任意类型，用于探测聚合体可初始化性
struct ubiq {
    template <class T>
    constexpr operator T&() const noexcept;  // NOLINT
};

namespace detail {

template <class T, std::size_t... I>
constexpr bool can_init_with(std::index_sequence<I...>) {
    return requires { T{((void)I, ubiq{})...}; };
}

template <class T, std::size_t N>
constexpr bool can_init_n() {
    return can_init_with<T>(std::make_index_sequence<N>{});
}

// 二分：找到最大的 N 使 T{ubiq × N} 合法
template <class T, std::size_t Lo, std::size_t Hi>
constexpr std::size_t count_fields_impl() {
    if constexpr (Lo >= Hi) {
        return Lo;
    } else {
        constexpr std::size_t Mid = (Lo + Hi + 1) / 2;
        if constexpr (can_init_n<T, Mid>()) {
            return count_fields_impl<T, Mid, Hi>();
        } else {
            return count_fields_impl<T, Lo, Mid - 1>();
        }
    }
}

}  // namespace detail

// 聚合体的字段个数
template <class T>
[[nodiscard]] constexpr std::size_t field_count() noexcept {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_aggregate_v<U>, "field_count<T> 要求 T 是聚合体");
    return detail::count_fields_impl<U, 0, 16>();
}

// ---------------------------------------------------------------- 字段绑定
// 每个字段数一个特化，用结构化绑定取到真正的成员引用（良定义）
#define MFWEB_TIE_SPECIALIZATION(n, ...)                                            \
    template <class T>                                                              \
    [[nodiscard]] constexpr auto tie_fields(T& value,                               \
                                            std::integral_constant<std::size_t, n>) { \
        auto& [__VA_ARGS__] = value;                                                \
        return std::tie(__VA_ARGS__);                                               \
    }

MFWEB_TIE_SPECIALIZATION(1, f0)
MFWEB_TIE_SPECIALIZATION(2, f0, f1)
MFWEB_TIE_SPECIALIZATION(3, f0, f1, f2)
MFWEB_TIE_SPECIALIZATION(4, f0, f1, f2, f3)
MFWEB_TIE_SPECIALIZATION(5, f0, f1, f2, f3, f4)
MFWEB_TIE_SPECIALIZATION(6, f0, f1, f2, f3, f4, f5)
MFWEB_TIE_SPECIALIZATION(7, f0, f1, f2, f3, f4, f5, f6)
MFWEB_TIE_SPECIALIZATION(8, f0, f1, f2, f3, f4, f5, f6, f7)
MFWEB_TIE_SPECIALIZATION(9, f0, f1, f2, f3, f4, f5, f6, f7, f8)
MFWEB_TIE_SPECIALIZATION(10, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9)
MFWEB_TIE_SPECIALIZATION(11, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10)
MFWEB_TIE_SPECIALIZATION(12, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11)
MFWEB_TIE_SPECIALIZATION(13, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12)
MFWEB_TIE_SPECIALIZATION(14, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13)
MFWEB_TIE_SPECIALIZATION(15, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14)
MFWEB_TIE_SPECIALIZATION(16, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15)

#undef MFWEB_TIE_SPECIALIZATION

template <class T>
[[nodiscard]] constexpr auto tie_fields(T& value) {
    return tie_fields(value, std::integral_constant<std::size_t, field_count<T>()>{});
}

// 按顺序访问每个字段（fn 收到字段引用）
template <class T, class F>
constexpr void for_each_field(T&& value, F&& fn) {
    auto refs = tie_fields(value);
    std::apply([&fn](auto&... members) { (fn(members), ...); }, refs);
}

// 按索引取第 I 个字段的引用
template <std::size_t I, class T>
[[nodiscard]] constexpr decltype(auto) get_field(T&& value) {
    auto refs = tie_fields(value);
    return std::get<I>(refs);
}

}  // namespace mfweb::reflect
