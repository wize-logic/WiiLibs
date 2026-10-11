# SPDX-License-Identifier: MIT
# wl_ctr: 3DS NCSD/NCCH/ExeFS/RomFS/GARC readers over a byte-source callback. Portable (wl_base,
# wl_nds for the LZ11 check); on host builds the file-backed source wl_ctr_src_open is added and
# wl_host is linked.
add_library(wl_ctr STATIC exefs.c garc.c ncch.c ncsd.c romfs.c src.c survey.c)
wl_target_defaults(wl_ctr)
target_link_libraries(wl_ctr PUBLIC wl_base wl_nds)
if(WL_PLATFORM STREQUAL "host")
    target_sources(wl_ctr PRIVATE src_file.c)
    target_link_libraries(wl_ctr PUBLIC wl_host)
endif()
add_library(WiiLibs::ctr ALIAS wl_ctr)
