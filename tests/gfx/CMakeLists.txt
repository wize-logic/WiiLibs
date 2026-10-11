# SPDX-License-Identifier: MIT
if(NOT TARGET wl_gfx)
    return()
endif()
foreach(t math ps tex_codec)
    wl_add_test(test_gfx_${t} SOURCES test_gfx_${t}.c LIBS wl_gfx LABELS unit be)
endforeach()
