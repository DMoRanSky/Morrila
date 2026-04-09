cmake_minimum_required(VERSION 3.20)
project(semantic_test)

set(CMAKE_CXX_STANDARD 17)

add_executable(test main.cpp)

# 包含 DuckDB 头文件
target_include_directories(test PUBLIC
    ../../src/include
)

# 链接 DuckDB 库
target_link_directories(test PUBLIC
    ../../build/release/src
)

target_link_libraries(test duckdb)
