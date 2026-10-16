# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxkrnl)
    return()
endif()
# The kernel through its imports, from guest functions written in C (no generated code).
wl_add_test(test_xboxkrnl SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
# The same in the runtime's deterministic mode (a virtual clock, thread.h).
wl_add_test(test_xboxkrnl_det SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
target_compile_definitions(test_xboxkrnl_det PRIVATE WL_XK_TEST_DETERMINISTIC=1)
# The same with guest memory paged over 1 MB of frames and a swap file (paging.c, the Wii's model).
wl_add_test(test_xboxkrnl_paging SOURCES test_xboxkrnl.c LIBS wl_xboxkrnl LABELS unit be)
target_compile_definitions(test_xboxkrnl_paging PRIVATE WL_XK_TEST_PAGING=1)
