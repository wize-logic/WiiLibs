# SPDX-License-Identifier: MIT
# wl_xbox: what a port of an Xbox (Direct3D 8) game needs of the NV2A on GX: its pixel shaders'
# register combiners translated to TEV programs (psh.h, with a cache of them: psh_cache.h) and its
# vertex programs decoded, analysed and run on the processor (vsh.h, with the programs compiled
# ahead of time by tools/xbox_vsh_codegen.c: vsh_native.h); and the NV2A as a device that runs a
# title's push buffers (nv2a.h: the MMIO registers, the pusher and puller, the 3D state, vertex
# fetch, captures), the textures converted for GX and kept, the processor's vertex pipeline and a
# recorder. Builds everywhere; no fused multiply-adds,
# so a vertex program gives the same bits on every target, interpreted or compiled.
add_library(wl_xbox STATIC psh.c psh_cache.c vsh.c
    nv2a.c nv2a_kelvin.c nv2a_capture.c nv2a_texture.c nv2a_texcache.c nv2a_vcache.c nv2a_pipeline.c nv2a_raster.c nv2a_record.c)
wl_target_defaults(wl_xbox DETERMINISTIC)
target_link_libraries(wl_xbox PUBLIC wl_platform wl_gfx wl_codec m)
add_library(WiiLibs::xbox ALIAS wl_xbox)

# wl_xbox_gx: the NV2A's draws on GX (nv2a_gx.h), through wl/platform's direct GX layer: libogc on the
# Wii, the host's recorder elsewhere
add_library(wl_xbox_gx STATIC nv2a_gx.c)
wl_target_defaults(wl_xbox_gx DETERMINISTIC)
target_link_libraries(wl_xbox_gx PUBLIC wl_xbox wl_platform)
add_library(WiiLibs::xbox_gx ALIAS wl_xbox_gx)

# the vertex program compiler, a host tool (it includes vsh.c for its decoder's internals), and the
# NV2A capture's replay (tools/xbox_nv2a_replay.c: the GX backend drawn by the host's software GX)
if(NOT WL_PLATFORM STREQUAL "wii")
    add_executable(xbox_nv2a_replay ${PROJECT_SOURCE_DIR}/tools/xbox_nv2a_replay.c)
    wl_target_defaults(xbox_nv2a_replay)
    target_link_libraries(xbox_nv2a_replay PRIVATE wl_xbox wl_xbox_gx wl_gxsoft wl_gxtex)
    add_executable(xbox_vsh_codegen ${PROJECT_SOURCE_DIR}/tools/xbox_vsh_codegen.c)
    wl_target_defaults(xbox_vsh_codegen DETERMINISTIC)
    target_link_libraries(xbox_vsh_codegen PRIVATE wl_gfx m)
endif()
