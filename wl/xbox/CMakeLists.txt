# SPDX-License-Identifier: MIT
# wl_xbox: what a port of an Xbox (Direct3D 8) game needs of the NV2A on GX: its pixel shaders'
# register combiners translated to TEV programs (psh.h, with a cache of them: psh_cache.h) and its
# vertex programs decoded, analysed and run on the processor (vsh.h, with the programs compiled
# ahead of time by tools/xbox_vsh_codegen.c: vsh_native.h). Builds everywhere; no fused
# multiply-adds, so a vertex program gives the same bits on every target, interpreted or compiled.
add_library(wl_xbox STATIC psh.c psh_cache.c vsh.c)
wl_target_defaults(wl_xbox DETERMINISTIC)
target_link_libraries(wl_xbox PUBLIC wl_platform wl_gfx m)
add_library(WiiLibs::xbox ALIAS wl_xbox)

# the vertex program compiler, a host tool (it includes vsh.c for its decoder's internals)
if(NOT WL_PLATFORM STREQUAL "wii")
    add_executable(xbox_vsh_codegen ${PROJECT_SOURCE_DIR}/tools/xbox_vsh_codegen.c)
    wl_target_defaults(xbox_vsh_codegen DETERMINISTIC)
    target_link_libraries(xbox_vsh_codegen PRIVATE wl_gfx m)
endif()
