# SPDX-License-Identifier: MIT
# wl_base: portable C11 basics (results, asserts, logging, arena, endian, fixed point, hashes,
# PRNG, UTF-8). No platform calls; builds everywhere.
add_library(wl_base STATIC
    base.c log.c arena.c fixed.c hash.c prng.c utf8.c version.c)
wl_target_defaults(wl_base DETERMINISTIC)
add_library(WiiLibs::base ALIAS wl_base)
