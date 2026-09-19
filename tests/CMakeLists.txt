# 单元测试。使用自研测试基座（见 include/mfweb/test/test.hpp）。
# 用例文件用 GLOB + CONFIGURE_DEPENDS，新增 test_*.cpp 时无需改动本文件。
file(GLOB MFWEB_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(mfweb_tests ${MFWEB_TEST_SOURCES})

target_link_libraries(mfweb_tests PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfweb_tests)

add_test(NAME mfweb_tests COMMAND mfweb_tests)
