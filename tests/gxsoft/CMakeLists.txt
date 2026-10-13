# SPDX-License-Identifier: MIT
if(NOT TARGET wl_gxsoft)
    return()
endif()
wl_add_test(test_gxsoft SOURCES test_gxsoft.c LIBS wl_gxsoft LABELS unit be)
