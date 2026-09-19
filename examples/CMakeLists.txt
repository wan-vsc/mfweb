# 示例程序。
add_executable(example_hello hello.cpp)
target_link_libraries(example_hello PRIVATE mfweb::mfweb)
mfweb_apply_common_options(example_hello)
