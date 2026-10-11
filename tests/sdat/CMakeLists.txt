# SPDX-License-Identifier: MIT
wl_add_test(test_sdat_archive SOURCES test_sdat_archive.c LIBS wl_sdat LABELS unit be)
if(WL_PLATFORM STREQUAL "host")
    wl_add_test(test_sdat_index SOURCES test_sdat_index.c LIBS wl_sdat)
endif()
