#include <mfweb/test/test.hpp>
#include <mfweb/util/result.hpp>

#include <memory>
#include <string>
#include <utility>

namespace {

// 用来验证析构次数的探针类型
struct lifetime_probe {
    static int alive;
    int id = 0;

    lifetime_probe() { ++alive; }
    explicit lifetime_probe(int v) : id(v) { ++alive; }
    lifetime_probe(const lifetime_probe& o) : id(o.id) { ++alive; }
    lifetime_probe(lifetime_probe&& o) noexcept : id(o.id) { ++alive; }
    lifetime_probe& operator=(const lifetime_probe&) = default;
    lifetime_probe& operator=(lifetime_probe&&) noexcept = default;
    ~lifetime_probe() { --alive; }
};

int lifetime_probe::alive = 0;

}  // namespace

MFW_TEST(result, ok_and_err_basic) {
    mfweb::result<int, std::string> good = mfweb::ok(7);
    MFW_CHECK(good.has_value());
    MFW_CHECK(static_cast<bool>(good));
    MFW_CHECK_EQ(good.value(), 7);
    MFW_CHECK_EQ(*good, 7);

    mfweb::result<int, std::string> bad = mfweb::err(std::string("boom"));
    MFW_CHECK(!bad.has_value());
    MFW_CHECK(!static_cast<bool>(bad));
    MFW_CHECK_EQ(bad.error(), std::string("boom"));
}

MFW_TEST(result, value_or_on_both_paths) {
    mfweb::result<int, std::string> good = mfweb::ok(7);
    mfweb::result<int, std::string> bad = mfweb::err(std::string("boom"));
    MFW_CHECK_EQ(good.value_or(99), 7);
    MFW_CHECK_EQ(bad.value_or(99), 99);
}

MFW_TEST(result, move_construct_and_assign) {
    mfweb::result<std::string, int> a = mfweb::ok(std::string("hello"));
    mfweb::result<std::string, int> b = std::move(a);
    MFW_CHECK(b.has_value());
    MFW_CHECK_EQ(b.value(), std::string("hello"));

    mfweb::result<std::string, int> c = mfweb::err(3);
    c = std::move(b);
    MFW_CHECK(c.has_value());
    MFW_CHECK_EQ(c.value(), std::string("hello"));
}

MFW_TEST(result, copy_is_independent) {
    mfweb::result<std::string, int> a = mfweb::ok(std::string("x"));
    mfweb::result<std::string, int> b = a;
    b.value() += "y";
    MFW_CHECK_EQ(a.value(), std::string("x"));
    MFW_CHECK_EQ(b.value(), std::string("xy"));
}

MFW_TEST(result, destructor_destroys_active_member_only) {
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
    {
        mfweb::result<lifetime_probe, int> r = mfweb::ok(lifetime_probe{1});
        MFW_CHECK_EQ(lifetime_probe::alive, 1);
        MFW_CHECK_EQ(r.value().id, 1);
    }
    MFW_CHECK_EQ(lifetime_probe::alive, 0);

    {
        mfweb::result<int, lifetime_probe> r = mfweb::err(lifetime_probe{2});
        MFW_CHECK_EQ(lifetime_probe::alive, 1);
        MFW_CHECK_EQ(r.error().id, 2);
    }
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
}

MFW_TEST(result, holds_move_only_value) {
    mfweb::result<std::unique_ptr<int>, int> r = mfweb::ok(std::make_unique<int>(5));
    MFW_CHECK(r.has_value());
    MFW_CHECK_EQ(*r.value(), 5);

    mfweb::result<std::unique_ptr<int>, int> moved = std::move(r);
    MFW_CHECK(moved.has_value());
    MFW_CHECK_EQ(*moved.value(), 5);
}

MFW_TEST(result, void_specialization) {
    mfweb::result<void, std::string> ok_r;
    MFW_CHECK(ok_r.has_value());

    mfweb::result<void, std::string> bad_r = mfweb::err(std::string("nope"));
    MFW_CHECK(!bad_r.has_value());
    MFW_CHECK_EQ(bad_r.error(), std::string("nope"));

    mfweb::result<void, std::string> moved = std::move(bad_r);
    MFW_CHECK(!moved.has_value());
    MFW_CHECK_EQ(moved.error(), std::string("nope"));
}

MFW_TEST(result, void_specialization_dtor_balance) {
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
    {
        mfweb::result<void, lifetime_probe> r = mfweb::err(lifetime_probe{9});
        MFW_CHECK_EQ(lifetime_probe::alive, 1);
    }
    MFW_CHECK_EQ(lifetime_probe::alive, 0);
}
