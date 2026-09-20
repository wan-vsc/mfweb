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
