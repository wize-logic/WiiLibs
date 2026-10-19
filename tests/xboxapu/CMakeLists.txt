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
# The same voices silenced on odd game ticks (a silent voice coming back as it would have played).
add_test(NAME bench_xboxapu_blink COMMAND bench_xboxapu 3000 b5b89d04)
set_tests_properties(bench_xboxapu_blink PROPERTIES LABELS "unit;be" ENVIRONMENT "BENCH_VOICES=blink")
add_test(NAME bench_xboxapu_plain COMMAND bench_xboxapu 3000 0cdf232f)
set_tests_properties(bench_xboxapu_plain PROPERTIES LABELS "unit;be" ENVIRONMENT "BENCH_PLAIN=1")
# Halo's a10 shape (bench_xboxapu.c, halo_voices): paused voices in the lists, 3D streams.
add_test(NAME bench_xboxapu_a10 COMMAND bench_xboxapu 3000 a3a5ef29)
set_tests_properties(bench_xboxapu_a10 PROPERTIES LABELS "unit;be" ENVIRONMENT "BENCH_VOICES=a10")
add_test(NAME bench_xboxapu_a10_plain COMMAND bench_xboxapu 3000 a3a5ef29)
set_tests_properties(bench_xboxapu_a10_plain PROPERTIES LABELS "unit;be" ENVIRONMENT "BENCH_VOICES=a10;BENCH_PLAIN=1")
