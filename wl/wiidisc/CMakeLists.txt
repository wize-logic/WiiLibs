# SPDX-License-Identifier: MIT
# wl_wiidisc: Wii disc extraction through dolphin-tool, with a hashed extraction cache. Host only
# (starts processes and walks the file system through wl_host).
if(NOT WL_PLATFORM STREQUAL "host")
    return()
endif()
add_library(wl_wiidisc STATIC wii.c)
wl_target_defaults(wl_wiidisc)
target_link_libraries(wl_wiidisc PUBLIC wl_base wl_host)
add_library(WiiLibs::wiidisc ALIAS wl_wiidisc)
