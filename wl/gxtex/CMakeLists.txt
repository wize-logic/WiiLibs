# SPDX-License-Identifier: MIT
# wl_gxtex: GX texture encoding / decoding (over wl_codec), Nintendo DS 2D resources (NCLR, NCGR, NCER, NANR),
# sprite sheets, DS TEX0 textures and a preview PNG writer. Meant for offline tools, but portable
# C with no platform calls, so it builds (and is tested big-endian) everywhere.
add_library(wl_gxtex STATIC gx.c ds2d.c sprite.c tex0.c)
wl_target_defaults(wl_gxtex)
target_link_libraries(wl_gxtex PUBLIC wl_codec wl_base)
add_library(WiiLibs::gxtex ALIAS wl_gxtex)
