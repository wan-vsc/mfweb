#include <mfweb/test/test.hpp>

namespace mfweb::test {

registry& registry::instance() {
    static registry r;
    return r;
}

void registry::add(std::string_view suite, std::string_view name, std::function<void()> fn) {
    cases_.push_back(case_info{suite, name, std::move(fn)});
}

void registry::fail(std::string_view file, int line, std::string_view expr, std::string_view detail) {
    ++failures_;
    std::printf("[FAIL] %.*s:%d\n        断言: %.*s\n", static_cast<int>(file.size()), file.data(),
                line, static_cast<int>(expr.size()), expr.data());
    if (!detail.empty()) {
        std::printf("        说明: %.*s\n", static_cast<int>(detail.size()), detail.data());
    }
}

int run_all(const char* filter) {
    auto& reg = registry::instance();
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    std::printf("=== mfweb test ===\n");
    for (const auto& c : reg.cases()) {
        std::string full;
        full.reserve(c.suite.size() + c.name.size() + 1);
        full.append(c.suite).append(".").append(c.name);

        if (filter != nullptr && full.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }

        const int before = reg.failure_count();
        std::printf("[ RUN  ] %s\n", full.c_str());
        try {
            c.fn();
        } catch (const std::exception& e) {
            reg.fail(__FILE__, __LINE__, "用例未捕获异常", e.what());
        } catch (...) {
            reg.fail(__FILE__, __LINE__, "用例未捕获异常", "未知异常类型");
        }

        if (reg.failure_count() == before) {
            ++passed;
            std::printf("[  OK  ] %s\n", full.c_str());
        } else {
            ++failed;
            std::printf("[ FAIL ] %s\n", full.c_str());
        }
    }

    std::printf("=== 用例 %d：通过 %d，失败 %d，跳过 %d ===\n", static_cast<int>(reg.cases().size()),
                passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

}  // namespace mfweb::test
