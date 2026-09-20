# 示例程序。
add_executable(example_hello hello.cpp)
target_link_libraries(example_hello PRIVATE mfweb::mfweb)
mfweb_apply_common_options(example_hello)

add_executable(static_server static_server.cpp)
target_link_libraries(static_server PRIVATE mfweb::mfweb)
mfweb_apply_common_options(static_server)

add_executable(then_chain then_chain.cpp)
target_link_libraries(then_chain PRIVATE mfweb::mfweb)
mfweb_apply_common_options(then_chain)

# ---- Qt 真实集成示例（P8/M6 的缺口）----------------------------------------
# 只在**真的装了 Qt** 的机器上构建：用 find_package 探测，找不到就整体跳过。
# 这样没装 Qt 的机器（以及 Linux 交付环境）不受影响。
find_package(Qt6 QUIET COMPONENTS Core)
if(Qt6_FOUND)
    message(STATUS "mfweb: 发现 Qt6 版本 ${Qt6_VERSION} —— 构建 qt_then_chain 真实集成示例")
    add_executable(qt_then_chain qt_then_chain.cpp)
    target_link_libraries(qt_then_chain PRIVATE mfweb::mfweb Qt6::Core)
    mfweb_apply_common_options(qt_then_chain)
else()
    message(STATUS "mfweb: 未找到 Qt6 —— 跳过 qt_then_chain（Qt 适配仍由 __has_include 门控，不影响其他目标）")
endif()
