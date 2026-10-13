cmake_minimum_required(VERSION 3.24)

set(CMAKE_C_COMPILER clang)
project(HumansRE C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)

add_executable(HumansRE src/main.c)
target_link_libraries(HumansRE PRIVATE SDL3::SDL3)
