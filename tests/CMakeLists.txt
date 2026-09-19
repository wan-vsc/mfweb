# 单元测试。使用自研测试基座（见 include/mfweb/test/test.hpp）。
# 用例文件用 GLOB + CONFIGURE_DEPENDS，新增 test_*.cpp 时无需改动本文件。
file(GLOB MFWEB_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(mfweb_tests ${MFWEB_TEST_SOURCES})

target_link_libraries(mfweb_tests PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfweb_tests)

add_test(NAME mfweb_tests COMMAND mfweb_tests)

# ---------------------------------------------------------------- 缺陷复现夹具
# 故意采用有缺陷的朴素设计，用来复现"连接超时导致的段错误"。
# **不作为 ctest 用例注册**：正常运行会崩溃是它的预期行为，只在 ASAN 配置下手动运行。
add_executable(mfweb_timeout_repro timeout_repro.cpp)
target_link_libraries(mfweb_timeout_repro PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfweb_timeout_repro)
