#pragma once

// mfweb::reflect —— 类内宏：指定要反射的字段，并支持字段别名。
//
// 用法（两个宏都写在类体内部）：
//
//     struct user {
//         std::int64_t id;
//         std::string name;
//         std::vector<std::string> tags;
//         std::string internal_note;      // 不想暴露
//
//         REFLECT_MEMBERS(id, name, tags);                // 只反射指定字段（**注意结尾分号**）
//         REFLECT_ALIASES("用户ID", "name", "tags");      // 可选；不写则用标识符名
//     };
//
// 设计要点：
//   * 宏不生成成员指针、不需要重复写类名 —— 直接 std::tie(字段列表)，
//     因此对私有成员也有效（宏在类内展开）。
//   * REFLECT_ALIASES 是可选的：不写时键名从 #__VA_ARGS__ 里按逗号切出来，
//     也就是字段的标识符本身。
//   * 两个宏的参数都是一整个可变参数包，不需要 Boost.PP 那套"逐元素展开"机器。
//
// 与无宏反射（aggregate.hpp）的关系：
//   * 无宏路径：字段个数 + 按位置取值（**拿不到名字**）；
//   * 类内宏：字段子集 + 名字 + 别名（JSON 用键名时必须走这条）。

#include <mfweb/reflect/aggregate.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>

namespace mfweb::reflect {

// 把可变参数包变成 string_view 数组（供 REFLECT_ALIASES 使用）
template <class... S>
[[nodiscard]] constexpr auto make_alias_array(S... s) noexcept {
    static_assert((std::is_convertible_v<S, std::string_view> && ...),
                  "别名必须是字符串字面量");
    return std::array<std::string_view, sizeof...(S)>{std::string_view{s}...};
}

// 从 "id, name, tags" 里取第 i 个名字（去两侧空白）
[[nodiscard]] constexpr std::string_view nth_name(std::string_view list,
                                                  std::size_t index) noexcept {
    std::size_t start = 0;
    for (std::size_t k = 0; k < index; ++k) {
        const std::size_t comma = list.find(',', start);
        if (comma == std::string_view::npos) { return {}; }
        start = comma + 1;
    }
    std::size_t end = list.find(',', start);
    if (end == std::string_view::npos) { end = list.size(); }
    while (start < end && (list[start] == ' ' || list[start] == '\t')) { ++start; }
    while (end > start && (list[end - 1] == ' ' || list[end - 1] == '\t')) { --end; }
    return list.substr(start, end - start);
}

template <class T>
concept has_reflect_members = requires { T::mfweb_has_reflect_members; };

template <class T>
concept has_reflect_aliases = requires { T::mfweb_has_reflect_aliases; };

// 该类型是否可用反射序列化（宏路径或"聚合体无宏路径"都算）
template <class T>
concept reflectable = has_reflect_members<T> ||
                      (std::is_aggregate_v<std::remove_cvref_t<T>> &&
                       !std::is_array_v<std::remove_cvref_t<T>> &&
                       !std::is_union_v<std::remove_cvref_t<T>>);

// 统一取"字段引用元组"
template <class T>
[[nodiscard]] constexpr auto reflect_tie(T& value) {
    if constexpr (has_reflect_members<std::remove_cvref_t<T>>) {
        return value.mfweb_tie();
    } else {
        return tie_fields(value);
    }
}

// 反射后的字段个数
template <class T>
[[nodiscard]] constexpr std::size_t reflect_field_count() {
    using U = std::remove_cvref_t<T>;
    if constexpr (has_reflect_members<U>) {
        return std::tuple_size_v<decltype(std::declval<U&>().mfweb_tie())>;
    } else {
        return field_count<U>();
    }
}

// 第 i 个字段的键名：优先别名，其次标识符（宏路径）；无宏路径返回空串
template <class T>
[[nodiscard]] std::string_view reflect_field_name(std::size_t index) {
    using U = std::remove_cvref_t<T>;
    if constexpr (has_reflect_aliases<U>) {
        const auto aliases = U::mfweb_alias_array();
        if (index < aliases.size() && !aliases[index].empty()) { return aliases[index]; }
    }
    if constexpr (has_reflect_members<U>) {
        return nth_name(U::mfweb_field_list(), index);
    }
    return {};
}

// 顺序访问反射字段
template <class T, class F>
constexpr void for_each_reflected_field(T&& value, F&& fn) {
    auto refs = reflect_tie(value);
    std::apply([&fn](auto&... members) { (fn(members), ...); }, refs);
}

}  // namespace mfweb::reflect

// ---------------------------------------------------------------- 类内宏

#define REFLECT_MEMBERS(...)                                                 \
    auto mfweb_tie() noexcept { return std::tie(__VA_ARGS__); }              \
    auto mfweb_tie() const noexcept { return std::tie(__VA_ARGS__); }        \
    static constexpr std::string_view mfweb_field_list() noexcept {          \
        return #__VA_ARGS__;                                                 \
    }                                                                        \
    static constexpr bool mfweb_has_reflect_members = true

#define REFLECT_ALIASES(...)                                                 \
    static constexpr auto mfweb_alias_array() noexcept {                     \
        return ::mfweb::reflect::make_alias_array(__VA_ARGS__);               \
    }                                                                        \
    static constexpr bool mfweb_has_reflect_aliases = true
