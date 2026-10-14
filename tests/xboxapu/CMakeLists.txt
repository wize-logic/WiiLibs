# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xboxapu)
    return()
endif()
# The APU and AC'97 models with synthetic voices in a synthetic guest memory (also big-endian).
wl_add_test(test_xboxapu SOURCES test_xboxapu.c LIBS wl_xboxapu LABELS unit be)
# The voice processor's benchmark (a scene shaped like a game's; tools/qemu_icount.py counts its
# instructions on ppc-be). Its checksum of every frame's mixbins and output over 3000 frames pins
# what the processor computes, bit for bit, on either byte order.
add_executable(bench_xboxapu bench_xboxapu.c)
wl_target_defaults(bench_xboxapu)
target_link_libraries(bench_xboxapu PRIVATE wl_xboxapu)
add_test(NAME bench_xboxapu_checksum COMMAND bench_xboxapu 3000 0cdf232f)
set_tests_properties(bench_xboxapu_checksum PROPERTIES LABELS "unit;be")
