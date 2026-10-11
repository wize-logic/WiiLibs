# SPDX-License-Identifier: MIT
# wl_codec: the GX texture codec (every GX texel format, TLUTs, CMPR with the hardware's 5/8 : 3/8
# blend), Xbox texture conversion to GX and the GameCube/Wii DSP-ADPCM decoder. Integer only, no allocation, no platform calls;
# builds everywhere with bit-identical output on little- and big-endian targets.
add_library(wl_codec STATIC gx_tex.c xbox_tex.c dsp_adpcm.c)
wl_target_defaults(wl_codec DETERMINISTIC)
target_link_libraries(wl_codec PUBLIC wl_base)
add_library(WiiLibs::codec ALIAS wl_codec)
