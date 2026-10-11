# SPDX-License-Identifier: MIT
# test_gcm reads images from memory (portable); test_gcm_file runs the same cases through files.
wl_add_test(test_gcm SOURCES test_gcm.c LIBS wl_gcm LABELS unit be)
if(WL_PLATFORM STREQUAL "host")
    wl_add_test(test_gcm_file SOURCES test_gcm.c LIBS wl_gcm wl_host)
    target_compile_definitions(test_gcm_file PRIVATE WL_GCM_TEST_FILE)
endif()
