# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxrt)
    return()
endif()
# Guest memory on the host / ppc-be backend, with the faults through the host's decoder.
wl_add_test(test_xboxrt_mem SOURCES test_xboxrt_mem.c LIBS wl_xboxrt LABELS unit be)
# The PowerPC and x86-64 decoders against a fake device (portable).
wl_add_test(test_xboxrt_decode SOURCES test_xboxrt_decode.c LIBS wl_xboxrt LABELS unit be)
