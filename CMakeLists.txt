cmake_minimum_required(VERSION 3.16)

project(file_manager_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(file_manager_server
    src/main.c
)

target_compile_options(file_manager_server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
