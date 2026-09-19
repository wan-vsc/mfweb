# 单元测试。使用自研测试基座（见 include/mfweb/test/test.hpp）。
add_executable(mfweb_tests
    test_main.cpp
    test_version.cpp
    test_toolchain.cpp
)

target_link_libraries(mfweb_tests PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfweb_tests)

add_test(NAME mfweb_tests COMMAND mfweb_tests)
