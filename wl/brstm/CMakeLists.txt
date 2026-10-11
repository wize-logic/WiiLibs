# SPDX-License-Identifier: MIT
# wl_brstm: NW4R audio readers (BRSTM streams, BRSAR sound archive index, RWSD wave sounds) and a
# DSP-ADPCM decoder (a wrapper over wl_codec). Portable; the file-path opener of BRSAR (wl_brsar_open) is added on host builds.
add_library(wl_brstm STATIC brstm.c brsar.c rwsd.c dsp.c)
wl_target_defaults(wl_brstm DETERMINISTIC)
target_link_libraries(wl_brstm PUBLIC wl_codec wl_base m)
if(WL_PLATFORM STREQUAL "host")
    target_sources(wl_brstm PRIVATE brsar_file.c)
    target_link_libraries(wl_brstm PUBLIC wl_host)
endif()
add_library(WiiLibs::brstm ALIAS wl_brstm)
