# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eye_demo_protocol_tests)

target_sources(app PRIVATE
    src/test_demo_protocol.c
    src/test_demo_image.c
    ../src/demo_protocol.c
    ../src/demo_image.c
    ../src/demo_view.c
)
target_include_directories(app PRIVATE ../src)
