# SPDX-License-Identifier: MIT
foreach(t stream brsar rwsd)
    wl_add_test(test_brstm_${t} SOURCES test_brstm_${t}.c LIBS wl_brstm LABELS unit be)
endforeach()
if(WL_PLATFORM STREQUAL "host")
    # the file-path opener (wl_brsar_open) exists on host builds only
    target_compile_definitions(test_brstm_brsar PRIVATE WL_BRSAR_FILE_TEST)
endif()
