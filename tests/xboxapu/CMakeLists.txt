# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxapu)
    return()
endif()
# The APU and AC'97 models with synthetic voices in a synthetic guest memory (also big-endian).
wl_add_test(test_xboxapu SOURCES test_xboxapu.c LIBS wl_xboxapu LABELS unit be)
