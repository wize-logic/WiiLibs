# SPDX-License-Identifier: MIT
# test_gxtex also links wl_gfx: its GX decoder cross-checks the encoded blocks.
if(NOT TARGET wl_gxtex OR NOT TARGET wl_gfx)
    return()
endif()
wl_add_test(test_gxtex SOURCES test_gxtex.c LIBS wl_gxtex wl_gfx LABELS unit be)
