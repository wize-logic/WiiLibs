# SPDX-License-Identifier: MIT
# wl_gs: Genius Sonority GSfsys.toc, FSYS archives and LZSS. Portable (wl_base); the header-only
# wl/gs/gs_file.h adapts a wl_host file on host builds.
add_library(wl_gs STATIC fsys.c lzss.c toc.c types.c)
wl_target_defaults(wl_gs)
target_link_libraries(wl_gs PUBLIC wl_base)
add_library(WiiLibs::gs ALIAS wl_gs)
