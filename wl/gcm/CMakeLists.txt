# SPDX-License-Identifier: MIT
# wl_gcm: GameCube disc image (GCM/ISO) reader over a read callback or a memory buffer.
# Portable (wl_base); on host builds wl_gcm_open(path) is added and wl_host is linked.
add_library(wl_gcm STATIC gcm.c)
wl_target_defaults(wl_gcm)
target_link_libraries(wl_gcm PUBLIC wl_base)
if(WL_PLATFORM STREQUAL "host")
    target_sources(wl_gcm PRIVATE gcm_file.c)
    target_link_libraries(wl_gcm PUBLIC wl_host)
endif()
add_library(WiiLibs::gcm ALIAS wl_gcm)
