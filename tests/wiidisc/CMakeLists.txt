# SPDX-License-Identifier: MIT
if(NOT WL_PLATFORM STREQUAL "host")
    return()
endif()
wl_add_test(test_wiidisc SOURCES test_wiidisc.c LIBS wl_wiidisc)
