# SPDX-License-Identifier: MIT
# wl_host: POSIX helpers for host tools (files, JSON, SHA-1, threads, processes). Host only.
if(NOT WL_PLATFORM STREQUAL "host")
    return()
endif()
find_package(Threads REQUIRED)
add_library(wl_host STATIC file.c json.c mem.c proc.c sha1.c thread.c)
wl_target_defaults(wl_host)
target_link_libraries(wl_host PUBLIC wl_base Threads::Threads)
add_library(WiiLibs::host ALIAS wl_host)
