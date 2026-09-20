// 编译期反射测试：无宏聚合体（字段计数 + 取值）、枚举反射、类内宏（子集 + 别名）。

#include <mfweb/reflect/aggregate.hpp>
#include <mfweb/reflect/enum.hpp>
#include <mfweb/reflect/macro.hpp>
#include <mfweb/test/test.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct point {
    int x = 0;
    int y = 0;
};

struct person {
    std::int64_t id = 0;
    std::string name;
    double score = 0.0;
    bool active = false;
};

struct empty_aggregate {};

enum class color { red, green = 5, blue };

enum class small_enum { a, b, c };

struct aliased {
    std::int64_t id = 7;
    std::string name = "n";
    std::string secret = "s";

    REFLECT_MEMBERS(id, name);
    REFLECT_ALIASES("用户ID", "name");
};

struct no_alias_macro {
    int alpha = 1;
    int beta = 2;

    REFLECT_MEMBERS(alpha, beta);
};

}  // namespace

MFW_TEST(reflect_aggregate, field_count) {
    MFW_CHECK_EQ(mfweb::reflect::field_count<point>(), static_cast<std::size_t>(2));
    MFW_CHECK_EQ(mfweb::reflect::field_count<person>(), static_cast<std::size_t>(4));
    MFW_CHECK_EQ(mfweb::reflect::field_count<empty_aggregate>(), static_cast<std::size_t>(0));
    MFW_CHECK_EQ(mfweb::reflect::field_count<aliased>(), static_cast<std::size_t>(3));
}

MFW_TEST(reflect_aggregate, get_field_by_index) {
    person p{42, "alice", 3.5, true};
    MFW_CHECK_EQ(mfweb::reflect::get_field<0>(p), static_cast<std::int64_t>(42));
    MFW_CHECK_EQ(mfweb::reflect::get_field<1>(p), std::string("alice"));
    MFW_CHECK_EQ(mfweb::reflect::get_field<2>(p), 3.5);
    MFW_CHECK_EQ(mfweb::reflect::get_field<3>(p), true);
}

MFW_TEST(reflect_aggregate, for_each_field_is_writable) {
    point p{1, 2};
    int sum = 0;
    mfweb::reflect::for_each_field(p, [&sum](auto& f) { sum += static_cast<int>(f); });
    MFW_CHECK_EQ(sum, 3);

    // 引用可写
    mfweb::reflect::for_each_field(p, [](auto& f) { f *= 10; });
    MFW_CHECK_EQ(p.x, 10);
    MFW_CHECK_EQ(p.y, 20);
}

MFW_TEST(reflect_aggregate, const_object_works) {
    const person p{1, "bob", 1.0, false};
    int count = 0;
    mfweb::reflect::for_each_field(p, [&count](const auto&) { ++count; });
    MFW_CHECK_EQ(count, 4);
}

MFW_TEST(reflect_enum, name_of_each_value) {
    MFW_CHECK_EQ(mfweb::reflect::enum_name(color::red), std::string_view("red"));
    MFW_CHECK_EQ(mfweb::reflect::enum_name(color::green), std::string_view("green"));
    MFW_CHECK_EQ(mfweb::reflect::enum_name(color::blue), std::string_view("blue"));
    MFW_CHECK_EQ(mfweb::reflect::enum_name(small_enum::b), std::string_view("b"));
}

MFW_TEST(reflect_enum, unknown_value_has_no_name) {
    // 未命名值（不在枚举声明里）取不到名字
    const auto bogus = static_cast<color>(99);
    MFW_CHECK_EQ(mfweb::reflect::enum_name(bogus), std::string_view());
}

MFW_TEST(reflect_enum, enum_values_lists_all_named) {
    const auto [values, n] = mfweb::reflect::enum_values<color>();
    MFW_CHECK_EQ(n, static_cast<std::size_t>(3));
    MFW_CHECK_EQ(values[0], color::red);
    MFW_CHECK_EQ(values[1], color::green);
    MFW_CHECK_EQ(values[2], color::blue);
}

MFW_TEST(reflect_macro, subset_of_fields) {
    aliased a;
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_count<aliased>(), static_cast<std::size_t>(2));

    int count = 0;
    mfweb::reflect::for_each_reflected_field(a, [&count](auto&) { ++count; });
    MFW_CHECK_MSG(count == 2, "宏只应反射列出的两个字段（secret 不应被反射）");
}

MFW_TEST(reflect_macro, aliases_take_precedence) {
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_name<aliased>(0), std::string_view("用户ID"));
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_name<aliased>(1), std::string_view("name"));
}

MFW_TEST(reflect_macro, names_fall_back_to_identifiers) {
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_name<no_alias_macro>(0), std::string_view("alpha"));
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_name<no_alias_macro>(1), std::string_view("beta"));
}

MFW_TEST(reflect_macro, no_macro_path_has_no_names) {
    // 无宏反射拿不到名字 —— 这是语言限制，不是实现偷懒
    MFW_CHECK_EQ(mfweb::reflect::reflect_field_name<point>(0), std::string_view());
}
