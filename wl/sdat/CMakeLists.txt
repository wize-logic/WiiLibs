# SPDX-License-Identifier: MIT
# wl_sdat: Nintendo DS SDAT sound archive reader (SSEQ, SSAR, SBNK, SWAR, STRM). Portable; the
# canonical index (index.h, SHA-1 through wl_host) is added on host builds.
add_library(wl_sdat STATIC sdat.c)
wl_target_defaults(wl_sdat)
target_link_libraries(wl_sdat PUBLIC wl_base)
if(WL_PLATFORM STREQUAL "host")
    target_sources(wl_sdat PRIVATE index.c)
    target_link_libraries(wl_sdat PUBLIC wl_host)
endif()
add_library(WiiLibs::sdat ALIAS wl_sdat)
