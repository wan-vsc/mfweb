cmake_minimum_required(VERSION 3.25)

project(mfweb
    VERSION 0.1.0
    DESCRIPTION "mfweb - 基于 C++20 协程的跨平台异步 Web 服务器框架"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 自定义配置 Asan。MSVC 的 /fsanitize=address 要求动态 CRT。
set(CMAKE_CXX_FLAGS_ASAN "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /fsanitize=address" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_ASAN "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "" FORCE)
if(CMAKE_BUILD_TYPE STREQUAL "Asan")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_definitions(MFWEB_ASAN=1)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)

option(MFWEB_BUILD_TESTS "构建单元测试" ON)
option(MFWEB_BUILD_BENCH "构建基准程序" ON)
option(MFWEB_BUILD_EXAMPLES "构建示例程序" ON)

# ---------------------------------------------------------------- 核心库
# 源文件用 GLOB_RECURSE + CONFIGURE_DEPENDS：新增 src/**.cpp 时自动纳入，无需改动本文件
file(GLOB_RECURSE MFWEB_LIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(mfweb STATIC ${MFWEB_LIB_SOURCES})
add_library(mfweb::mfweb ALIAS mfweb)

target_include_directories(mfweb
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

mfweb_apply_common_options(mfweb)

if(WIN32)
    target_link_libraries(mfweb PUBLIC ws2_32 mswsock)
endif()

# ---------------------------------------------------------------- 子目录
if(MFWEB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MFWEB_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(MFWEB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

message(STATUS "mfweb ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
