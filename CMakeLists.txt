# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_ccsds_eye_demo)

target_sources(app PRIVATE
    src/main.c
    src/demo_ui.c
    src/demo_protocol.c
    src/demo_image.c
    src/demo_view.c
    src/demo_sdls_feedback.c
    src/demo_camera.c
    src/demo_service.c
)
