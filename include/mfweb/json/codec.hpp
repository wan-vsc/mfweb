#pragma once

// mfweb::json —— 反射驱动的编解码（to_string / from_string 的实现）。
//
// 类型分派（编译期 if constexpr，无虚函数、无类型擦除）：
//   bool / 整数 / 浮点      ↔ JSON boolean / number
//   std::string / string_view ↔ JSON string
//   枚举                     ↔ JSON string（用 reflect::enum_name 双向映射）
//   vector<T> / list<T>      ↔ JSON array
//   optional<T>              ↔ null 或值
//   map<string,T>            ↔ JSON object
//   **有类内宏**的类型        ↔ JSON object（键名来自宏，含别名）
//   **无宏聚合体**            ↔ JSON array（按字段顺序；因为拿不到字段名）
//
// 未知键默认忽略；缺失键保持目标对象的默认值（不视为错误）。这两条策略在 web 场景
// 下最实用：向前兼容（服务端加字段不该让老客户端解析失败）。

#include <mfweb/json/json.hpp>
#include <mfweb/reflect/enum.hpp>
#include <mfweb/reflect/macro.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <type_traits>
#include <vector>

namespace mfweb::json {
namespace detail {

template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
struct is_map : std::false_type {};
template <class T, class C, class A>
struct is_map<std::map<std::string, T, C, A>> : std::true_type {};

template <class T>
inline constexpr bool is_string_like =
    std::is_same_v<std::remove_cvref_t<T>, std::string> ||
    std::is_same_v<std::remove_cvref_t<T>, std::string_view>;

void append_number_json(std::string& out, double d);
void append_escaped_json(std::string& out, std::string_view s);

// 是否有"键名"（只有类内宏路径有）
template <class T>
inline constexpr bool has_names = mfweb::reflect::has_reflect_members<std::remove_cvref_t<T>>;

template <class T>
void write_any(std::string& out, const T& v) {
    using U = std::remove_cvref_t<T>;

    if constexpr (std::is_same_v<U, bool>) {
        out += v ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<U>) {
        append_number_json(out, static_cast<double>(v));
    } else if constexpr (is_string_like<U>) {
        append_escaped_json(out, std::string_view(v));
    } else if constexpr (std::is_same_v<U, value>) {
        write_value(out, v);
    } else if constexpr (std::is_enum_v<U>) {
        const std::string_view name = mfweb::reflect::enum_name(v);
        if (name.empty()) {
            append_number_json(out, static_cast<double>(static_cast<std::underlying_type_t<U>>(v)));
        } else {
            append_escaped_json(out, name);
        }
    } else if constexpr (is_optional<U>::value) {
        if (v.has_value()) {
            write_any(out, *v);
        } else {
            out += "null";
        }
    } else if constexpr (is_vector<U>::value) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : v) {
            if (!first) { out.push_back(','); }
            first = false;
            write_any(out, item);
        }
        out.push_back(']');
    } else if constexpr (is_map<U>::value) {
        out.push_back('{');
        bool first = true;
        for (const auto& [k, item] : v) {
            if (!first) { out.push_back(','); }
            first = false;
            append_escaped_json(out, k);
            out.push_back(':');
            write_any(out, item);
        }
        out.push_back('}');
    } else if constexpr (mfweb::reflect::reflectable<U>) {
        if constexpr (has_names<U>) {
            // 宏路径：对象形式，键名来自宏（含别名）
            out.push_back('{');
            std::size_t index = 0;
            bool first = true;
            auto refs = mfweb::reflect::reflect_tie(v);
            std::apply(
                [&](const auto&... members) {
                    const auto emit = [&](const auto& member) {
                        if (!first) { out.push_back(','); }
                        first = false;
                        append_escaped_json(out, mfweb::reflect::reflect_field_name<U>(index));
                        out.push_back(':');
                        write_any(out, member);
                        ++index;
                    };
                    (emit(members), ...);
                },
                refs);
            out.push_back('}');
        } else {
            // 无宏路径：数组形式（拿不到字段名）
            out.push_back('[');
            bool first = true;
            mfweb::reflect::for_each_reflected_field(v, [&](const auto& member) {
                if (!first) { out.push_back(','); }
                first = false;
                write_any(out, member);
            });
            out.push_back(']');
        }
    } else {
        static_assert(sizeof(U) == 0, "json::to_string 不支持该类型");
    }
}

template <class T>
bool read_any(const value& v, T& out, error& err) {
    using U = std::remove_cvref_t<T>;

    if constexpr (std::is_same_v<U, bool>) {
        if (!v.is_bool()) { err.message = "期望 boolean"; return false; }
        out = v.as_bool();
        return true;
    } else if constexpr (std::is_arithmetic_v<U>) {
        if (!v.is_number()) { err.message = "期望 number"; return false; }
        out = static_cast<U>(v.as_number());
        return true;
    } else if constexpr (std::is_same_v<U, std::string>) {
        if (!v.is_string()) { err.message = "期望 string"; return false; }
        out = v.as_string();
        return true;
    } else if constexpr (std::is_same_v<U, value>) {
        out = v;
        return true;
    } else if constexpr (std::is_enum_v<U>) {
        if (v.is_string()) {
            const std::string_view want = v.as_string();
            const auto [values, n] = mfweb::reflect::enum_values<U>();
            for (std::size_t i = 0; i < n; ++i) {
                if (mfweb::reflect::enum_name(values[i]) == want) {
                    out = values[i];
                    return true;
                }
            }
            err.message = "未知枚举名: " + std::string(want);
            return false;
        }
        if (v.is_number()) {
            out = static_cast<U>(static_cast<std::underlying_type_t<U>>(v.as_int()));
            return true;
        }
        err.message = "枚举需要 string 或 number";
        return false;
    } else if constexpr (is_optional<U>::value) {
        if (v.is_null()) {
            out.reset();
            return true;
        }
        typename U::value_type inner{};
        if (!read_any(v, inner, err)) { return false; }
        out = std::move(inner);
        return true;
    } else if constexpr (is_vector<U>::value) {
        if (!v.is_array()) { err.message = "期望 array"; return false; }
        out.clear();
        out.reserve(v.as_array().size());
        for (const auto& item : v.as_array()) {
            typename U::value_type element{};
            if (!read_any(item, element, err)) { return false; }
            out.push_back(std::move(element));
        }
        return true;
    } else if constexpr (is_map<U>::value) {
        if (!v.is_object()) { err.message = "期望 object"; return false; }
        out.clear();
        for (const auto& [k, item] : v.as_object()) {
            typename U::mapped_type mapped{};
            if (!read_any(item, mapped, err)) { return false; }
            out.emplace(k, std::move(mapped));
        }
        return true;
    } else if constexpr (mfweb::reflect::reflectable<U>) {
        if constexpr (has_names<U>) {
            if (!v.is_object()) { err.message = "期望 object"; return false; }
            std::size_t index = 0;
            bool ok = true;
            auto refs = mfweb::reflect::reflect_tie(out);
            std::apply(
                [&](auto&... members) {
                    const auto try_one = [&](auto& member) {
                        if (!ok) { return; }
                        const std::string_view name =
                            mfweb::reflect::reflect_field_name<U>(index);
                        ++index;
                        if (const value* found = v.find(name)) {
                            if (!read_any(*found, member, err)) { ok = false; }
                        }
                        // 未知键忽略；缺失键保持默认值（见文件头策略说明）
                    };
                    (try_one(members), ...);
                },
                refs);
            return ok;
        } else {
            if (!v.is_array()) { err.message = "无宏聚合体需要 array 形式"; return false; }
            std::size_t index = 0;
            bool ok = true;
            const auto& items = v.as_array();
            mfweb::reflect::for_each_reflected_field(out, [&](auto& member) {
                if (!ok) { return; }
                if (index < items.size()) {
                    if (!read_any(items[index], member, err)) { ok = false; }
                }
                ++index;
            });
            return ok;
        }
    } else {
        static_assert(sizeof(U) == 0, "json::from_string 不支持该类型");
    }
}

}  // namespace detail

template <class T>
std::string to_string(const T& v) {
    std::string out;
    detail::write_any(out, v);
    return out;
}

template <class T>
result<T, error> from_string(std::string_view text) {
    auto parsed = parse(text);
    if (!parsed) { return mfweb::err(std::move(parsed.error())); }
    T out{};
    error err{};
    if (!detail::read_any(parsed.value(), out, err)) { return mfweb::err(std::move(err)); }
    return mfweb::ok(std::move(out));
}

}  // namespace mfweb::json
