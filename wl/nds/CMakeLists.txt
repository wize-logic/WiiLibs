# SPDX-License-Identifier: MIT
# wl_nds: Nintendo DS ROM (header, NitroFS FNT/FAT, overlays), NARC archives and the LZ10/LZ11/BLZ
# decoders. Portable (wl_base only); builds everywhere.
add_library(wl_nds STATIC fnt.c lz.c narc.c nds_rom.c survey.c)
wl_target_defaults(wl_nds)
target_link_libraries(wl_nds PUBLIC wl_base)
add_library(WiiLibs::nds ALIAS wl_nds)
