# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxkrnl)
    return()
endif()
# The kernel through its imports, from guest functions written in C (no generated code).
wl_add_test(test_xboxkrnl SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
# The same in the runtime's deterministic mode (a virtual clock, thread.h).
wl_add_test(test_xboxkrnl_det SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
target_compile_definitions(test_xboxkrnl_det PRIVATE WL_XK_TEST_DETERMINISTIC=1)
