# SPDX-License-Identifier: MIT
if(NOT TARGET wl_xbox)
    return()
endif()
wl_add_test(test_xbox_psh SOURCES test_xbox_psh.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_psh_cache SOURCES test_xbox_psh_cache.c LIBS wl_xbox LABELS unit be)
wl_add_test(test_xbox_nv2a SOURCES test_xbox_nv2a.c LIBS wl_xbox LABELS unit be)
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
