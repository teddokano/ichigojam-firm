cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ichigojam_z C)

set(IJB_BASIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../IchigoJam_BASIC)

target_sources(app PRIVATE
    src/main.c
    ${IJB_BASIC_DIR}/main.c
)

target_include_directories(app PRIVATE
    src
    ${IJB_BASIC_DIR}
)
