# SPDX-License-Identifier: MIT
# wl_gfx: vector / matrix / camera maths, paired-single skinning routines (Broadway inline assembly
# on the Wii, plain C elsewhere), a correctly rounded sqrtf for Broadway (gfx_sqrt.h) and GX texture
# encoders / decoders (wrappers over wl_codec). Builds
# everywhere; depends on wl_codec, libc and libm.
add_library(wl_gfx STATIC gfx_math.c gfx_ps.c gfx_sqrt.c gfx_sqrt_wrap.c tex_codec.c)
wl_target_defaults(wl_gfx)
wl_gpu_path(wl_gfx)
target_link_libraries(wl_gfx PUBLIC wl_codec m)
add_library(WiiLibs::gfx ALIAS wl_gfx)
