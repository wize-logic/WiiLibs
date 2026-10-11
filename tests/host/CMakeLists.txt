# SPDX-License-Identifier: MIT
if(NOT WL_PLATFORM STREQUAL "host")
    return()
endif()
wl_add_test(test_host_base SOURCES test_host_base.c LIBS wl_host)
