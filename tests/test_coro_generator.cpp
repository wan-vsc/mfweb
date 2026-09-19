#include <mfweb/coro/generator.hpp>
#include <mfweb/test/test.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

mfweb::coro::generator<int> range(int from, int to) {
    for (int i = from; i < to; ++i) { co_yield i; }
}

mfweb::coro::generator<int> empty() { co_return; }

mfweb::coro::generator<std::string> words() {
    co_yield std::string("a");
    co_yield std::string("bb");
    co_yield std::string("ccc");
}

mfweb::coro::generator<int> throws_at_third(bool& reached_second) {
    co_yield 1;
    reached_second = true;
    co_yield 2;
    throw std::runtime_error("generator boom");
}

struct counter {
    static int live;
    int v = 0;
    counter() { ++live; }
    explicit counter(int x) : v(x) { ++live; }
    counter(const counter& o) : v(o.v) { ++live; }
    counter(counter&& o) noexcept : v(o.v) { ++live; }
    counter& operator=(const counter&) = default;
    counter& operator=(counter&&) noexcept = default;
    ~counter() { --live; }
};

int counter::live = 0;

mfweb::coro::generator<counter> counters(int n) {
    for (int i = 0; i < n; ++i) { co_yield counter{i}; }
}

}  // namespace

MFW_TEST(coro_generator, iterates_all_values) {
    std::vector<int> got;
    for (int v : range(0, 5)) { got.push_back(v); }
    MFW_CHECK_EQ(got.size(), static_cast<std::size_t>(5));
    for (int i = 0; i < 5; ++i) { MFW_CHECK_EQ(got[static_cast<std::size_t>(i)], i); }
}

MFW_TEST(coro_generator, empty_yields_nothing) {
    int count = 0;
    for (int v : empty()) {
        (void)v;
        ++count;
    }
    MFW_CHECK_EQ(count, 0);
}

MFW_TEST(coro_generator, is_lazy_until_first_element) {
    bool started = false;
    auto gen = [&started]() -> mfweb::coro::generator<int> {
        started = true;
        co_yield 1;
    }();

    MFW_CHECK_MSG(!started, "generator 在创建时就执行了，惰性语义被破坏");

    auto it = gen.begin();
    MFW_CHECK_MSG(started, "generator 取首个元素后仍未执行");
    MFW_CHECK_EQ(*it, 1);
}

MFW_TEST(coro_generator, holds_non_trivial_values) {
    MFW_CHECK_EQ(counter::live, 0);
    {
        std::vector<int> got;
        for (const counter& c : counters(4)) { got.push_back(c.v); }
        MFW_CHECK_EQ(got.size(), static_cast<std::size_t>(4));
        MFW_CHECK_EQ(got[3], 3);
    }
    MFW_CHECK_EQ(counter::live, 0);
}

MFW_TEST(coro_generator, strings_are_moved_not_copied_incorrectly) {
    std::vector<std::string> got;
    for (const std::string& w : words()) { got.push_back(w); }
    MFW_CHECK_EQ(got.size(), static_cast<std::size_t>(3));
    MFW_CHECK_EQ(got[0], std::string("a"));
    MFW_CHECK_EQ(got[2], std::string("ccc"));
}

MFW_TEST(coro_generator, exception_surfaces_at_the_right_position) {
    bool reached_second = false;
    auto gen = throws_at_third(reached_second);

    auto it = gen.begin();
    MFW_CHECK_EQ(*it, 1);
    ++it;
    MFW_CHECK_EQ(*it, 2);
    MFW_CHECK_MSG(reached_second, "第二个值未被产出");

    MFW_CHECK_THROWS(++it);
}

MFW_TEST(coro_generator, early_break_destroys_generator_safely) {
    MFW_CHECK_EQ(counter::live, 0);
    {
        int seen = 0;
        for (const counter& c : counters(100)) {
            (void)c;
            ++seen;
            if (seen == 3) { break; }  // 提前跳出：生成器必须被安全析构
        }
        MFW_CHECK_EQ(seen, 3);
    }
    MFW_CHECK_EQ(counter::live, 0);
}

MFW_TEST(coro_generator, movable_but_not_copyable) {
    static_assert(!std::is_copy_constructible_v<mfweb::coro::generator<int>>);
    static_assert(std::is_move_constructible_v<mfweb::coro::generator<int>>);

    auto a = range(0, 3);
    auto b = std::move(a);
    MFW_CHECK(b.valid());

    int sum = 0;
    for (int v : b) { sum += v; }
    MFW_CHECK_EQ(sum, 3);
}

MFW_TEST(coro_generator, destroyed_without_iteration_is_safe) {
    {
        auto gen = range(0, 1000);
        MFW_CHECK(gen.valid());
    }
    MFW_CHECK(true);
}
