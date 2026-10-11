# SPDX-License-Identifier: MIT
wl_add_test(test_ctr SOURCES test_ctr.c LIBS wl_ctr LABELS unit be)
# wl_ctr_src_open (src_file.c) exists on host builds only.
if(WL_PLATFORM STREQUAL "host")
    wl_add_test(test_ctr_src SOURCES test_ctr_src.c LIBS wl_ctr wl_host LABELS unit)
endif()
