# SPDX-License-Identifier: MIT
wl_add_test(test_base_endian SOURCES test_base_endian.c LIBS wl_base LABELS unit be)
if(WL_PLATFORM STREQUAL "ppc-be")
    target_compile_definitions(test_base_endian PRIVATE WL_EXPECT_BIG_ENDIAN)
endif()
foreach(t arena hash prng utf8 fixed assert)
    wl_add_test(test_base_${t} SOURCES test_base_${t}.c LIBS wl_base LABELS unit be)
endforeach()
