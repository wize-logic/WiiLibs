# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxkrnl)
    return()
endif()
# The kernel through its imports, from guest functions written in C (no generated code).
wl_add_test(test_xboxkrnl SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
