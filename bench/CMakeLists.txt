# 基准程序。所有基准以 CSV 输出到 stdout，便于脚本收集与绘图。
add_executable(mfbench
    mfbench.cpp
    coro_bench.cpp
    timer_bench.cpp
    router_bench.cpp
    http_bench.cpp
    blocking_bench.cpp
)

target_link_libraries(mfbench PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfbench)

if(WIN32)
    target_link_libraries(mfbench PRIVATE psapi)
endif()
