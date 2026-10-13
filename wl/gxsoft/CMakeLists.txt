# SPDX-License-Identifier: MIT
# wl_gxsoft: a software GX for the host (gxsoft.h): what a program asks of wl/platform's GX layer
# (the direct layer, TEV programs, render targets, frames) drawn on the processor as the console's
# GX draws it, through the host backend's renderer hook. Host and ppc-be (it needs that backend).
if(WL_PLATFORM STREQUAL "wii")
    return()
endif()
add_library(wl_gxsoft STATIC gxsoft.c)
wl_target_defaults(wl_gxsoft DETERMINISTIC)
target_link_libraries(wl_gxsoft PUBLIC wl_platform wl_codec m)
add_library(WiiLibs::gxsoft ALIAS wl_gxsoft)
