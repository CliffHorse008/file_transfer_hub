cmake_minimum_required(VERSION 3.16)

project(file_transfer_hub C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(file_transfer_hub
    src/main.c
)

target_compile_options(file_transfer_hub PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(file_transfer_hub PRIVATE Threads::Threads)
