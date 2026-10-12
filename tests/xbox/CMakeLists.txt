# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xbox)
    return()
endif()
wl_add_test(test_xbox_psh SOURCES test_xbox_psh.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_psh_cache SOURCES test_xbox_psh_cache.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a SOURCES test_xbox_nv2a.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_kelvin SOURCES test_xbox_nv2a_kelvin.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_texture SOURCES test_xbox_nv2a_texture.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a_gx SOURCES test_xbox_nv2a_gx.c LIBS wl_xbox_gx LABELS unit be)
wl_add_test(test_xbox_nv2a_raster SOURCES test_xbox_nv2a_raster.c LIBS wl_xbox LABELS unit be)
# the replay tool over the capture the rasterizer's test writes
if(WL_PLATFORM STREQUAL "host")
    set(nv2a_capture ${CMAKE_CURRENT_BINARY_DIR}/nv2a_test.capture)
    set_tests_properties(test_xbox_nv2a_raster PROPERTIES ENVIRONMENT "WL_NV2A_CAPTURE_OUT=${nv2a_capture}"
        FIXTURES_SETUP nv2a_capture)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay)
    add_test(NAME xbox_nv2a_replay COMMAND xbox_nv2a_replay ${nv2a_capture} -v --png ${CMAKE_CURRENT_BINARY_DIR}/nv2a_replay)
    set_tests_properties(xbox_nv2a_replay PROPERTIES LABELS unit FIXTURES_REQUIRED nv2a_capture
        PASS_REGULAR_EXPRESSION "2 draws.*frame_0000.png|frame_0000.png.*2 draws")
endif()
wl_add_test(test_xbox_vsh SOURCES test_xbox_vsh.c LIBS wl_xbox LABELS unit be)
target_include_directories(test_xbox_vsh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# the synthetic programs (vsh_programs.inc) compiled ahead of time by tools/xbox_vsh_codegen.c (run
# under the cross-compiling emulator on ppc-be), against the interpreter
set(vsh_generated ${CMAKE_CURRENT_BINARY_DIR}/vsh_programs.c)
add_custom_command(OUTPUT ${vsh_generated}
    COMMAND xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc ${vsh_generated} test_vsh_natives
    DEPENDS xbox_vsh_codegen ${CMAKE_CURRENT_SOURCE_DIR}/vsh_programs.inc
    VERBATIM)
wl_add_test(test_xbox_vsh_native SOURCES test_xbox_vsh_native.c ${vsh_generated} LIBS wl_xbox LABELS unit be)
target_include_directories(test_xbox_vsh_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
