#pragma once

// mfweb::test —— 自研极简测试基座。
//
// 设计取舍：本项目运行时零第三方依赖，测试框架也不引入 GoogleTest / Catch2。
// 本基座只做三件事：注册用例、运行用例、报告失败位置。断言失败不抛异常而是记录，
// 以便一次运行看到全部失败点。
//
// 用法：
//     #include <mfweb/test/test.hpp>
//     MFW_TEST(util_result, basic_ok) {
//         MFW_CHECK(1 + 1 == 2);
//         MFW_CHECK_EQ(1, 1);
//     }
//     MFW_TEST_MAIN()

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mfweb::test {

struct case_info {
    std::string_view suite;
    std::string_view name;
    std::function<void()> fn;
};

class registry {
public:
    static registry& instance();

    void add(std::string_view suite, std::string_view name, std::function<void()> fn);

    [[nodiscard]] const std::vector<case_info>& cases() const noexcept { return cases_; }

    void fail(std::string_view file, int line, std::string_view expr, std::string_view detail);

    [[nodiscard]] int failure_count() const noexcept { return failures_; }

private:
    std::vector<case_info> cases_;
    int failures_ = 0;
};

// 运行全部用例（可按名字子串过滤），返回进程退出码：0 = 全部通过
int run_all(const char* filter = nullptr);

}  // namespace mfweb::test

// ---------------------------------------------------------------- 宏

#define MFW_CONCAT_IMPL(a, b) a##b
#define MFW_CONCAT(a, b) MFW_CONCAT_IMPL(a, b)

#define MFW_TEST(suite, name)                                                          \
    static void MFW_CONCAT(mfw_test_fn_, __LINE__)();                                  \
    namespace {                                                                        \
    struct MFW_CONCAT(mfw_test_reg_, __LINE__) {                                       \
        MFW_CONCAT(mfw_test_reg_, __LINE__)() {                                        \
            ::mfweb::test::registry::instance().add(                                   \
                #suite, #name, &MFW_CONCAT(mfw_test_fn_, __LINE__));                   \
        }                                                                              \
    } MFW_CONCAT(mfw_test_reg_inst_, __LINE__);                                        \
    }                                                                                  \
    static void MFW_CONCAT(mfw_test_fn_, __LINE__)()

#define MFW_CHECK(expr)                                                                \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            ::mfweb::test::registry::instance().fail(__FILE__, __LINE__, #expr, "");   \
        }                                                                              \
    } while (false)

#define MFW_CHECK_MSG(expr, msg)                                                       \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            ::mfweb::test::registry::instance().fail(__FILE__, __LINE__, #expr, (msg)); \
        }                                                                              \
    } while (false)

#define MFW_CHECK_EQ(actual, expected)                                                 \
    do {                                                                               \
        const auto& mfw_a = (actual);                                                  \
        const auto& mfw_b = (expected);                                                \
        if (!(mfw_a == mfw_b)) {                                                       \
            ::mfweb::test::registry::instance().fail(__FILE__, __LINE__,               \
                                                     #actual " == " #expected,         \
                                                     "值不相等");                      \
        }                                                                              \
    } while (false)

#define MFW_CHECK_THROWS(expr)                                                         \
    do {                                                                               \
        bool mfw_threw = false;                                                        \
        try {                                                                          \
            (void)(expr);                                                              \
        } catch (...) {                                                                \
            mfw_threw = true;                                                          \
        }                                                                              \
        if (!mfw_threw) {                                                              \
            ::mfweb::test::registry::instance().fail(__FILE__, __LINE__,               \
                                                     #expr " 应抛异常", "");           \
        }                                                                              \
    } while (false)

#define MFW_TEST_MAIN()                                                                \
    int main(int argc, char** argv) {                                                  \
        return ::mfweb::test::run_all(argc > 1 ? argv[1] : nullptr);                   \
    }
