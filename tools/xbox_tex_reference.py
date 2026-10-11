#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reference for wl/codec/xbox_tex: the OpenCE-Wii port's Python converter (tools/wii_gx_textures.py).

The converter in C (wl_xbox_*) has to produce, byte for byte, what that Python does for the same
Xbox texture. This script builds synthetic Xbox textures (no game data) from a fixed generator,
converts them with the Python, and either

  --emit FILE      writes tests/codec/xbox_tex_cases.inc: each case's inputs and the CRC-32 of the
                   Python's output (what test_xbox_tex.c checks on every platform), or
  --compare DIR    compares the files the C test dumped (WL_XBOX_DUMP_DIR=DIR, host build) with the
                   Python's output, byte for byte.

The Python lives in the OpenCE-Wii repository: --port DIR is its tools folder (default: the checkout
beside this one). Needs numpy. The input generator below is repeated in tests/codec/test_xbox_tex.c.
"""
import argparse
import os
import struct
import sys
import zlib

# (name, bytes per texel / per block, DXT?) in the order of WlXboxFormat
FORMATS = ["a8r8g8b8", "x8r8g8b8", "r5g6b5", "a1r5g5b5", "a4r4g4b4", "l8", "a8", "al8", "a8l8", "p8",
           "dxt1", "dxt3", "dxt5"]
SWIZZLED_SIZES = [(32, 32), (64, 16), (8, 128), (1, 1), (2, 4), (16, 1), (4, 32)]
LINEAR_SIZES = [(20, 12, 0), (20, 12, 12), (4, 4, 0), (33, 9, 0), (1, 5, 0), (7, 8, 3)]
DXT_SIZES = [(32, 32), (36, 20), (4, 4), (1, 1), (2, 13), (8, 64), (64, 8), (5, 5)]


def lcg_bytes(seed, count):
    out = bytearray(count)
    x = seed & 0xFFFFFFFF
    for i in range(count):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def make_input(kind, size, seed):
    data = bytearray(lcg_bytes(seed, size))
    if kind.startswith("dxt"):
        block = 8 if kind == "dxt1" else 16
        colour = 0 if kind == "dxt1" else 8
        for n in range(size // block):
            base = n * block + colour
            if n % 5 == 0:  # equal endpoints
                data[base + 2], data[base + 3] = data[base], data[base + 1]
            elif n % 5 == 1:  # c0 just above c1 (same high byte)
                data[base + 1], data[base + 3] = data[base + 3], data[base + 3]
                data[base] = (data[base + 2] + 1) & 0xFF
    return bytes(data)


def cases():
    """(index, kind, w, h, swizzled, pitch)"""
    out = []
    for kind in FORMATS:
        if kind.startswith("dxt"):
            out += [(kind, w, h, 0, 0) for w, h in DXT_SIZES]
        else:
            out += [(kind, w, h, 1, 0) for w, h in SWIZZLED_SIZES]
            bpp = {"a8r8g8b8": 4, "x8r8g8b8": 4, "r5g6b5": 2, "a1r5g5b5": 2, "a4r4g4b4": 2, "a8l8": 2}.get(kind, 1)
            # a linear level always has an explicit pitch here (the Python's pitch 0 means swizzled);
            # extra 0 is a packed level, which the C side takes as pitch 0 or w * bpp alike
            out += [(kind, w, h, 0, w * bpp + extra) for w, h, extra in LINEAR_SIZES]
    return [(i,) + c for i, c in enumerate(out)]


def python_outputs(wgt, kind, w, h, swizzled, pitch, data):
    """(main bytes, alpha bytes or None) as the port converts level 0"""
    colour = (-(-w // 8)) * (-(-h // 8)) * 32
    wgt.gx_levels = lambda *a, **k: 1  # keep the level whatever the Xbox texture's size
    # the real convert(), with slack at the end so its in-place size check passes
    xbox_pitch = 0 if swizzled else pitch
    out = wgt.convert(data + bytes(1 << 16), kind, w, h, 1, xbox_pitch)
    if kind.startswith("dxt"):
        if kind == "dxt1":
            return out[:colour], None
        return out[:colour], out[colour:2 * colour]
    size = wgt.gx_level_bytes(kind, w, h, 0)
    return out[:size], None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.path.join(os.path.dirname(__file__), "..", "..", "OpenCE-Wii", "tools"))
    ap.add_argument("--emit")
    ap.add_argument("--compare")
    args = ap.parse_args()
    sys.path.insert(0, args.port)
    import wii_gx_textures as wgt  # noqa: E402
    lines = []
    bad = 0
    for index, kind, w, h, swizzled, pitch in cases():
        seed = (0x1000193 * (index + 1)) & 0xFFFFFFFF
        wgt_pitch = pitch
        size = wgt.xbox_level_bytes(kind, w, h, 0, wgt_pitch)
        data = make_input(kind, size, seed)
        main_bytes, alpha = python_outputs(wgt, kind, w, h, swizzled, pitch, data)
        fmt_index = FORMATS.index(kind)
        lines.append("    {%d, %d, %d, %d, %d, 0x%08Xu, %d, 0x%08Xu, %d, 0x%08Xu}, // %s %dx%d%s" % (
            fmt_index, w, h, swizzled, pitch, seed, len(main_bytes), zlib.crc32(main_bytes),
            len(alpha) if alpha else 0, zlib.crc32(alpha) if alpha else 0, kind, w, h,
            " swizzled" if swizzled else (" pitch %d" % pitch if pitch else "")))
        if args.compare:
            for suffix, expect in (("", main_bytes), (".alpha", alpha)):
                if expect is None:
                    continue
                path = os.path.join(args.compare, "case%03d%s.bin" % (index, suffix))
                got = open(path, "rb").read()
                if got != expect:
                    bad += 1
                    print("DIFFERENT", path, kind, w, h, swizzled, pitch, len(got), len(expect))
    if args.emit:
        with open(args.emit, "w") as f:
            f.write("// SPDX-License-Identifier: MIT\n// Generated by tools/xbox_tex_reference.py from the OpenCE-Wii port's wii_gx_textures.py;"
                    " do not edit.\n// { format, w, h, swizzled, pitch, seed, output bytes, output CRC-32, alpha bytes, alpha CRC-32 }\n")
            f.write("\n".join(lines) + "\n")
        print("wrote", len(lines), "cases to", args.emit)
    if args.compare:
        print("compared %d cases, %d different" % (len(lines), bad))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
