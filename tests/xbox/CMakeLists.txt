# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xbox)
    return()
endif()
wl_add_test(test_xbox_psh SOURCES test_xbox_psh.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_psh_cache SOURCES test_xbox_psh_cache.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_psh_bias SOURCES test_xbox_psh_bias.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a SOURCES test_xbox_nv2a.c LIBS wl_xbox LABELS unit be)
# the register numbers against another public table, when one is checked out (test-only, read as text)
set(WL_XBOXRECOMP_DIR "$ENV{HOME}/GitHub/xboxrecomp" CACHE PATH "an xboxrecomp checkout for test_xbox_nv2a_regs")
wl_add_test(test_xbox_nv2a_regs SOURCES test_xbox_nv2a_regs.c LIBS wl_xbox LABELS unit be)
target_compile_definitions(test_xbox_nv2a_regs PRIVATE
    WL_NV2A_REGS_MINE="${PROJECT_SOURCE_DIR}/wl/xbox/nv2a_regs.h"
    WL_NV2A_REGS_THEIRS="${WL_XBOXRECOMP_DIR}/src/nv2a/nv2a_regs.h")
wl_add_test(test_xbox_nv2a_kelvin SOURCES test_xbox_nv2a_kelvin.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_texture SOURCES test_xbox_nv2a_texture.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_vcache SOURCES test_xbox_nv2a_vcache.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_gx SOURCES test_xbox_nv2a_gx.c LIBS wl_xbox_gx LABELS unit be)
# the backend's vertex prefetch (dcbt on the Wii): nv2a_gx.c and nv2a_kelvin.c (the fetch plans' areas) built
# again with each line asked for recorded (WL_NV2A_PREFETCH_RECORD), on every target (wl_xbox's own
# nv2a_kelvin object is then never linked)
wl_add_test(test_xbox_nv2a_prefetch SOURCES test_xbox_nv2a_prefetch.c ${PROJECT_SOURCE_DIR}/wl/xbox/nv2a_gx.c
    ${PROJECT_SOURCE_DIR}/wl/xbox/nv2a_kelvin.c
    LIBS wl_xbox wl_platform LABELS unit be)
wl_target_defaults(test_xbox_nv2a_prefetch DETERMINISTIC)
target_compile_definitions(test_xbox_nv2a_prefetch PRIVATE WL_NV2A_PREFETCH_RECORD)
# the ring of made vertices' slots (nv2a_gx.c's made_place, made_slots, made_flush): the file built into
# the test itself, its static functions driven draw by draw
wl_add_test(test_xbox_nv2a_ring SOURCES test_xbox_nv2a_ring.c LIBS wl_xbox wl_platform LABELS unit be)
wl_target_defaults(test_xbox_nv2a_ring DETERMINISTIC)
# the raster state's mappings from values alone (nv2a_raster.c), linked without the rest of wl_xbox as a
# Direct3D device's harness links it (OpenCE-Wii's draw bench)
wl_add_test(test_xbox_nv2a_raster SOURCES test_xbox_nv2a_raster.c ${PROJECT_SOURCE_DIR}/wl/xbox/nv2a_raster.c
    LIBS wl_base LABELS unit be)
# the GX backend drawn by the host's software GX (wl/gxsoft): the console suite's frames and pixel
# tests; and the replay tool over the capture it writes
if(TARGET wl_gxsoft)
    wl_add_test(test_xbox_nv2a_pixels SOURCES test_xbox_nv2a_pixels.c suite_nv2a.c LIBS wl_xbox_gx wl_gxsoft
        LABELS unit be)
endif()
if(WL_PLATFORM STREQUAL "host")
    set(nv2a_capture ${CMAKE_CURRENT_BINARY_DIR}/nv2a_test.capture)
    set_tests_properties(test_xbox_nv2a_pixels PROPERTIES ENVIRONMENT "WL_NV2A_CAPTURE_OUT=${nv2a_capture}"
        FIXTURES_SETUP nv2a_capture)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay)
    add_test(NAME xbox_nv2a_replay COMMAND xbox_nv2a_replay ${nv2a_capture} -v --png-gx ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay
        --programs ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay/programs.inc)
    set_tests_properties(xbox_nv2a_replay PROPERTIES LABELS unit FIXTURES_REQUIRED nv2a_capture
        PASS_REGULAR_EXPRESSION "frame_0000.png.*2 draws.*software GX: [0-9]+ primitives")
    # the replay streamed into a caller's memory (as the runtime's capture bench makes it) is the same
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay_stream)
    add_test(NAME xbox_nv2a_replay_stream COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:xbox_nv2a_replay>
        -DCAPTURE=${nv2a_capture} -DDIR=${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay_stream
        -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_stream.cmake)
    set_tests_properties(xbox_nv2a_replay_stream PROPERTIES LABELS unit FIXTURES_REQUIRED nv2a_capture)
endif()
wl_add_test(test_xbox_vsh SOURCES test_xbox_vsh.c LIBS wl_xbox LABELS unit be)
target_include_directories(test_xbox_vsh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# the synthetic programs (vsh_programs.inc) compiled ahead of time by tools/xbox_vsh_codegen.c (run
# under the cross-compiling emulator on ppc-be), against the interpreter
set(vsh_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_programs.c)
add_custom_command(OUTPUT ${vsh_generated}
    COMMAND xbox_vsh_codegen --colors-positions ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${vsh_generated} test_vsh_natives
    DEPENDS xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc
    VERBATIM)
# the batch interpreter against the plain one, on random programs
wl_add_test(test_xbox_vsh_batch SOURCES test_xbox_vsh_batch.c LIBS wl_xbox LABELS unit be)
target_include_directories(test_xbox_vsh_batch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
wl_add_test(test_xbox_vsh_native SOURCES test_xbox_vsh_native.c ${vsh_generated} LIBS wl_xbox LABELS unit be)
target_include_directories(test_xbox_vsh_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# the same programs' paired-single code, the Wii's (WL_XBOX_VSH_PS), emulated in C
# (vsh_ps_emulate.py, on the build machine), against the interpreter within the console suite's
# tolerance (suite_vsh_ps.c, which the console runs on the Gekko)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(vsh_ps_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_ps_programs.c)
    set(vsh_ps_emulated ${CMAKE_CURRENT_BINARY_DIR}/vsh_ps_emulated.c)
    add_custom_command(OUTPUT ${vsh_ps_emulated}
        COMMAND xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${vsh_ps_generated} test_vsh_ps_natives
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py ${vsh_ps_generated} ${vsh_ps_emulated}
        DEPENDS xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py
        VERBATIM)
    set_source_files_properties(${vsh_ps_emulated} PROPERTIES COMPILE_OPTIONS "-w")
    wl_add_test(test_xbox_vsh_ps SOURCES test_xbox_vsh_ps.c suite_vsh_ps.c ${vsh_ps_emulated} LIBS wl_xbox LABELS unit be)
    target_include_directories(test_xbox_vsh_ps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # a fused batch (--fused vsh_fused_forms.txt) emulated as Broadway runs it, against the general
    # path's words
    set(vsh_fused_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_fused_programs.c)
    set(vsh_fused_emulated ${CMAKE_CURRENT_BINARY_DIR}/vsh_fused_emulated.c)
    add_custom_command(OUTPUT ${vsh_fused_emulated}
        COMMAND xbox_vsh_codegen --fused ${CMAKE_CURRENT_SOURCE_DIR}/vsh_fused_forms.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${vsh_fused_generated} test_vsh_fused_natives
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py ${vsh_fused_generated} ${vsh_fused_emulated}
        DEPENDS xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${CMAKE_CURRENT_SOURCE_DIR}/vsh_fused_forms.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py
        VERBATIM)
    set_source_files_properties(${vsh_fused_emulated} PROPERTIES COMPILE_OPTIONS "-w;-ffp-contract=off")
    wl_add_test(test_xbox_vsh_fused SOURCES test_xbox_vsh_fused.c ${vsh_fused_emulated} LIBS wl_xbox LABELS unit be)
    # (and with --spill: a skinned form's kept blend in memory, as the forms whose registers run out)
    set(vsh_spill_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_spill_programs.c)
    set(vsh_spill_emulated ${CMAKE_CURRENT_BINARY_DIR}/vsh_spill_emulated.c)
    add_custom_command(OUTPUT ${vsh_spill_emulated}
        COMMAND xbox_vsh_codegen --spill --fused ${CMAKE_CURRENT_SOURCE_DIR}/vsh_fused_forms.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${vsh_spill_generated} test_vsh_fused_natives
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py ${vsh_spill_generated} ${vsh_spill_emulated}
        DEPENDS xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${CMAKE_CURRENT_SOURCE_DIR}/vsh_fused_forms.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/vsh_ps_emulate.py
        VERBATIM)
    set_source_files_properties(${vsh_spill_emulated} PROPERTIES COMPILE_OPTIONS "-w;-ffp-contract=off")
    wl_add_test(test_xbox_vsh_fused_spill SOURCES test_xbox_vsh_fused.c ${vsh_spill_emulated} LIBS wl_xbox LABELS unit be)
endif()
