#include <mfweb/test/test.hpp>
#include <mfweb/version.hpp>

#include <string>

MFW_TEST(version, fields_are_consistent) {
    const auto v = mfweb::version();
    MFW_CHECK_EQ(v.major, 0);
    MFW_CHECK_EQ(v.minor, 1);
    MFW_CHECK_EQ(v.patch, 0);
    MFW_CHECK(!v.stage.empty());
    MFW_CHECK(!v.compiler.empty());
}

MFW_TEST(version, string_matches) {
    MFW_CHECK_EQ(std::string(mfweb::version_string()), std::string("0.1.0-dev"));
}

MFW_TEST(version, compiler_is_detected) {
    // 本机 Windows 侧只使用 MSVC
#if defined(_MSC_VER)
    MFW_CHECK_EQ(std::string(mfweb::version().compiler), std::string("MSVC"));
#endif
}
