// JSON 测试：DOM/解析/序列化 + 反射驱动编解码。

#include <mfweb/json/codec.hpp>
#include <mfweb/test/test.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

struct user {
    std::int64_t id = 0;
    std::string name;
    std::vector<std::string> tags;

    REFLECT_MEMBERS(id, name, tags);
    REFLECT_ALIASES("用户ID", "name", "tags");
};

struct plain_point {
    int x = 0;
    int y = 0;
};

struct nested {
    std::string title;
    user owner;
    std::optional<int> score;

    REFLECT_MEMBERS(title, owner, score);
};

enum class role { admin, guest, banned };

struct with_enum {
    role r = role::guest;
    std::map<std::string, int> counters;

    REFLECT_MEMBERS(r, counters);
};

}  // namespace

MFW_TEST(json, parse_scalars) {
    MFW_CHECK(mfweb::json::parse("null")->is_null());
    MFW_CHECK_EQ(mfweb::json::parse("true")->as_bool(), true);
    MFW_CHECK_EQ(mfweb::json::parse("42")->as_int(), static_cast<std::int64_t>(42));
    MFW_CHECK_EQ(mfweb::json::parse("-3.5")->as_number(), -3.5);
    MFW_CHECK_EQ(mfweb::json::parse("1e2")->as_number(), 100.0);
    // 注意：必须先把 result 存下来再取字符串。
    // parse(...)->as_string() 返回的是"临时 result 内部字符串"的引用；
    // MFW_CHECK_EQ 会把它绑到 const auto&，临时对象在该全表达式结束时即销毁 → 悬垂。
    // （as_bool()/as_number()/as_int() 按值返回，所以上面的写法没问题。）
    const auto text = mfweb::json::parse("\"hi\"");
    MFW_CHECK(static_cast<bool>(text));
    MFW_CHECK_EQ(text.value().as_string(), std::string("hi"));
}

MFW_TEST(json, parse_nested_structures) {
    const auto r = mfweb::json::parse(R"({"a":[1,2,{"b":true}],"c":{"d":"e"}})");
    MFW_CHECK(static_cast<bool>(r));
    const auto& v = r.value();
    MFW_CHECK(v.is_object());
    const auto* a = v.find("a");
    MFW_CHECK(a != nullptr && a->is_array());
    MFW_CHECK_EQ(a->as_array().size(), static_cast<std::size_t>(3));
    const auto* inner = v.find("c");
    MFW_CHECK(inner != nullptr);
    MFW_CHECK_EQ(inner->find("d")->as_string(), std::string("e"));
}

MFW_TEST(json, parse_unicode_escapes) {
    const auto r = mfweb::json::parse(R"("\u4e2d\u6587")");
    MFW_CHECK(static_cast<bool>(r));
    MFW_CHECK_EQ(r.value().as_string(), std::string("中文"));

    // 代理对：U+1F600
    const auto emoji = mfweb::json::parse(R"("\ud83d\ude00")");
    MFW_CHECK(static_cast<bool>(emoji));
    MFW_CHECK_EQ(emoji.value().as_string().size(), static_cast<std::size_t>(4));  // UTF-8 4 字节
}

MFW_TEST(json, parse_errors_report_position) {
    const auto r = mfweb::json::parse("{\n  \"a\": }\n}");
    MFW_CHECK_MSG(!static_cast<bool>(r), "非法 JSON 应当报错");
    MFW_CHECK_EQ(r.error().line, static_cast<std::size_t>(2));
    MFW_CHECK(!r.error().message.empty());
}

MFW_TEST(json, parse_depth_limit) {
    std::string deep;
    for (int i = 0; i < 200; ++i) { deep.push_back('['); }
    for (int i = 0; i < 200; ++i) { deep.push_back(']'); }
    const auto r = mfweb::json::parse(deep, 128);
    MFW_CHECK_MSG(!static_cast<bool>(r), "超过深度上限应当报错而不是栈溢出");
}

MFW_TEST(json, serialize_escapes_and_roundtrip) {
    mfweb::json::value v;
    v.set("text", std::string("line1\nline2\t\"quoted\" \\ backslash"));
    v.set("num", 1.5);
    v.set("flag", true);
    std::string arr;
    mfweb::json::value::array_t items;
    items.push_back(mfweb::json::value{1});
    items.push_back(mfweb::json::value{std::string("two")});
    v.set("list", mfweb::json::value{std::move(items)});

    const std::string text = mfweb::json::serialize(v);
    const auto back = mfweb::json::parse(text);
    MFW_CHECK_MSG(static_cast<bool>(back), "序列化结果必须能重新解析");
    MFW_CHECK_EQ(back.value().find("text")->as_string(),
                 std::string("line1\nline2\t\"quoted\" \\ backslash"));
    MFW_CHECK_EQ(back.value().find("list")->as_array().size(), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------- 反射驱动

MFW_TEST(json_reflect, macro_object_with_aliases) {
    user u{7, "alice", {"a", "b"}};
    const std::string text = mfweb::json::to_string(u);
    // 别名 "用户ID" 应当出现，而不是标识符 "id"
    MFW_CHECK(text.find("\"用户ID\":7") != std::string::npos);
    MFW_CHECK(text.find("\"name\":\"alice\"") != std::string::npos);
    MFW_CHECK(text.find("\"tags\":[\"a\",\"b\"]") != std::string::npos);
    MFW_CHECK_MSG(text.find("\"id\"") == std::string::npos, "别名应当取代标识符作为键名");
}

MFW_TEST(json_reflect, roundtrip_user) {
    user original{42, "bob", {"x", "y", "z"}};
    const std::string text = mfweb::json::to_string(original);
    const auto parsed = mfweb::json::from_string<user>(text);
    MFW_CHECK_MSG(static_cast<bool>(parsed), "反序列化失败");
    MFW_CHECK_EQ(parsed.value().id, static_cast<std::int64_t>(42));
    MFW_CHECK_EQ(parsed.value().name, std::string("bob"));
    MFW_CHECK_EQ(parsed.value().tags.size(), static_cast<std::size_t>(3));
    MFW_CHECK_EQ(parsed.value().tags[2], std::string("z"));
}

MFW_TEST(json_reflect, no_macro_aggregate_uses_array_form) {
    plain_point p{3, 4};
    MFW_CHECK_EQ(mfweb::json::to_string(p), std::string("[3,4]"));

    const auto back = mfweb::json::from_string<plain_point>("[5,6]");
    MFW_CHECK(static_cast<bool>(back));
    MFW_CHECK_EQ(back.value().x, 5);
    MFW_CHECK_EQ(back.value().y, 6);
}

MFW_TEST(json_reflect, nested_and_optional) {
    nested n{"title", user{1, "n", {"t"}}, std::nullopt};
    const std::string text = mfweb::json::to_string(n);
    MFW_CHECK(text.find("\"score\":null") != std::string::npos);

    auto parsed = mfweb::json::from_string<nested>(text);
    MFW_CHECK(static_cast<bool>(parsed));
    MFW_CHECK_EQ(parsed.value().title, std::string("title"));
    MFW_CHECK_EQ(parsed.value().owner.name, std::string("n"));
    MFW_CHECK_MSG(!parsed.value().score.has_value(), "null 应解析为 nullopt");

    auto with_score = mfweb::json::from_string<nested>(
        R"({"title":"t","owner":{"用户ID":9,"name":"z","tags":[]},"score":88})");
    MFW_CHECK(static_cast<bool>(with_score));
    MFW_CHECK_EQ(with_score.value().score.value(), 88);
    MFW_CHECK_EQ(with_score.value().owner.id, static_cast<std::int64_t>(9));
}

MFW_TEST(json_reflect, enums_and_maps) {
    with_enum w{role::admin, {{"hits", 3}, {"miss", 1}}};
    const std::string text = mfweb::json::to_string(w);
    MFW_CHECK(text.find("\"r\":\"admin\"") != std::string::npos);
    MFW_CHECK(text.find("\"hits\":3") != std::string::npos);

    const auto back = mfweb::json::from_string<with_enum>(text);
    MFW_CHECK(static_cast<bool>(back));
    MFW_CHECK_EQ(back.value().r, role::admin);
    MFW_CHECK_EQ(back.value().counters.at("hits"), 3);
}

MFW_TEST(json_reflect, unknown_keys_ignored_missing_keys_keep_defaults) {
    // 向前兼容策略：多余键忽略；缺失键保持默认值
    const auto r = mfweb::json::from_string<user>(
        R"({"用户ID":5,"name":"n","tags":["t"],"brand_new_field":123})");
    MFW_CHECK_MSG(static_cast<bool>(r), "未知键不应导致解析失败");
    MFW_CHECK_EQ(r.value().id, static_cast<std::int64_t>(5));

    const auto partial = mfweb::json::from_string<user>(R"({"name":"only"})");
    MFW_CHECK(static_cast<bool>(partial));
    MFW_CHECK_EQ(partial.value().id, static_cast<std::int64_t>(0));  // 默认值
    MFW_CHECK_EQ(partial.value().name, std::string("only"));
}

MFW_TEST(json_reflect, type_mismatch_reports_error) {
    const auto r = mfweb::json::from_string<user>(R"({"name":42})");
    MFW_CHECK_MSG(!static_cast<bool>(r), "类型不匹配应当报错");
    MFW_CHECK(!r.error().message.empty());
}
