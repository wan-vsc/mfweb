#pragma once

// mfweb::json —— 自研 JSON DOM、解析器与序列化器，并支持反射驱动的自动转换。
//
// 设计取舍：
//   * **对象用"保序的键值对数组"** 而不是哈希表：web 场景下对象通常只有几个键，
//     线性查找足够快；保序让序列化结果稳定可预期（便于测试与人工比对）。
//   * 数字统一按 double 存；取整数用 as_int()。这不是任意精度实现，超出
//     ±2^53 的整数会有精度损失 —— 明确记录为限制，不做无谓的"看起来精确"。
//   * 解析器是**递归下降 + 深度上限**（默认 128）。JSON 本身是递归结构，
//     深度上限是防栈溢出的必要措施；超限返回错误而不是崩溃。
//   * 序列化用 std::to_chars 输出浮点（最短往返表示），解析用 std::from_chars。
//
// 与反射的关系（reflect/）：
//   * 类内宏 REFLECT_MEMBERS + REFLECT_ALIASES → 对象形式，键名来自宏（含别名）；
//   * 无宏聚合体 → **数组形式**（字段顺序即元素顺序）。因为 C++20 拿不到字段名，
//     拿不到名字就不可能生成有意义的键 —— 这是语言限制，写在这里免得被误解。

#include <mfweb/util/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mfweb::json {

class value {
public:
    using array_t = std::vector<value>;
    using object_t = std::vector<std::pair<std::string, value>>;

    enum class kind : std::uint8_t { null, boolean, number, string, array, object };

    value() noexcept = default;
    value(std::nullptr_t) noexcept {}
    value(bool b) noexcept : data_(b) {}
    value(double d) noexcept : data_(d) {}
    value(int i) noexcept : data_(static_cast<double>(i)) {}
    value(std::int64_t i) noexcept : data_(static_cast<double>(i)) {}
    value(std::string s) : data_(std::move(s)) {}
    value(const char* s) : data_(std::string(s)) {}
    value(array_t a) : data_(std::move(a)) {}
    value(object_t o) : data_(std::move(o)) {}

    [[nodiscard]] kind type() const noexcept { return static_cast<kind>(data_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return type() == kind::null; }
    [[nodiscard]] bool is_bool() const noexcept { return type() == kind::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type() == kind::number; }
    [[nodiscard]] bool is_string() const noexcept { return type() == kind::string; }
    [[nodiscard]] bool is_array() const noexcept { return type() == kind::array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == kind::object; }

    [[nodiscard]] bool as_bool() const noexcept { return std::get<bool>(data_); }
    [[nodiscard]] double as_number() const noexcept { return std::get<double>(data_); }
    [[nodiscard]] std::int64_t as_int() const noexcept {
        return static_cast<std::int64_t>(std::get<double>(data_));
    }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data_); }
    [[nodiscard]] const array_t& as_array() const { return std::get<array_t>(data_); }
    [[nodiscard]] array_t& as_array() { return std::get<array_t>(data_); }
    [[nodiscard]] const object_t& as_object() const { return std::get<object_t>(data_); }
    [[nodiscard]] object_t& as_object() { return std::get<object_t>(data_); }

    // 对象成员查找；不存在返回 nullptr
    [[nodiscard]] const value* find(std::string_view key) const {
        if (!is_object()) { return nullptr; }
        for (const auto& [k, v] : as_object()) {
            if (k == key) { return &v; }
        }
        return nullptr;
    }

    void set(std::string key, value v) {
        if (!is_object()) { data_ = object_t{}; }
        as_object().emplace_back(std::move(key), std::move(v));
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string, array_t, object_t> data_{nullptr};
};

struct error {
    std::size_t line = 1;
    std::size_t column = 1;
    std::string message;

    [[nodiscard]] std::string to_string() const {
        return "第 " + std::to_string(line) + " 行第 " + std::to_string(column) +
               " 列: " + message;
    }
};

[[nodiscard]] result<value, error> parse(std::string_view text,
                                         std::size_t max_depth = 128) noexcept;

[[nodiscard]] std::string serialize(const value& v);

// ---------------------------------------------------------------- 反射驱动

namespace detail {
void write_value(std::string& out, const value& v);
}  // namespace detail

// 任意可反射类型 → JSON 文本
template <class T>
[[nodiscard]] std::string to_string(const T& v);

// JSON 文本 → 任意可反射类型
template <class T>
[[nodiscard]] result<T, error> from_string(std::string_view text);

}  // namespace mfweb::json
