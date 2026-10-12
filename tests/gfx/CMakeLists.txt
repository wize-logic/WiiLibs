# SPDX-License-Identifier: MIT
if(NOT TARGET wl_gfx)
    return()
endif()
foreach(t math ps sqrt tex_codec)
    wl_add_test(test_gfx_${t} SOURCES test_gfx_${t}.c LIBS wl_gfx LABELS unit be)
endforeach()
# a program that wraps sqrtf
wl_add_test(test_gfx_sqrt_wrap SOURCES test_gfx_sqrt_wrap.c LIBS wl_gfx LABELS unit be)
wl_gfx_wrap_sqrtf(test_gfx_sqrt_wrap)
# test_gfx_sqrt counts the calls to wl_sqrtf_integer that wl_sqrtf_refine leaves to it
target_link_options(test_gfx_sqrt PRIVATE "LINKER:--wrap=wl_sqrtf_integer")
