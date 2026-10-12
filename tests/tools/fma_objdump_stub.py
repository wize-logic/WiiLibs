#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A stand-in for a PowerPC objdump, for testing wl_add_fma_check on host builds: it lists every
file it is given as a PowerPC object with a function `clean_fn`, and, where the file's path has
`fused` in it, a function `fused_fn` with a fmadd. Arguments: [-M OPTIONS] -d --no-show-raw-insn FILE..."""

import sys

args = sys.argv[1:]
if "-d" not in args:
    sys.exit("fma_objdump_stub.py: expected -d")
files = [arg for arg in args if not arg.startswith("-") and arg not in ("gekko", "altivec,vsx")]
for path in files:
    print(f"\n{path}:     file format elf32-powerpc\n\n\nDisassembly of section .text:\n")
    print("00000000 <clean_fn>:\n   0:\tfmul    f1,f1,f2\n   4:\tblr\n")
    if "fused" in path:
        print("00000010 <fused_fn>:\n  10:\tfmadd   f1,f1,f2,f3\n  14:\tblr")
