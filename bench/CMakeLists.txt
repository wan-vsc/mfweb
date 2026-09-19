# 基准程序。P0 仅为占位，P1 起逐个补入可复现的基准。
add_executable(mfbench mfbench.cpp)
target_link_libraries(mfbench PRIVATE mfweb::mfweb)
mfweb_apply_common_options(mfbench)
