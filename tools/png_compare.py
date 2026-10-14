#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compares two folders of PNG frames (xbox_nv2a_replay --png-gx's, say) with a tolerance.

For each frame of the same name in both: the pixels whose channels differ at all, and those that
differ by more than the threshold in any channel (red, green, blue, alpha, 0..255), the largest
difference, and where the differing pixels lie (their bounds). With --diff DIR, an image of each
frame's differences: black where equal, the difference scaled up elsewhere, red beyond the threshold.

    png_compare.py A_DIR B_DIR [--threshold N] [--diff DIR]

Exit status: 0 when no frame has a pixel beyond the threshold, 1 otherwise, 2 bad usage. Only 8-bit
RGB or RGBA, non-interlaced PNGs (what wl/codec's encoder writes) are read.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    """(width, height, channels, rows of pixels unfiltered)"""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, width = 8, [], 0
    height = depth = color = interlace = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    channels = {2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError(f"{path}: only 8-bit RGB or RGBA, non-interlaced")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def compare(a: Path, b: Path, threshold: int, diff_dir: Path | None) -> tuple[int, str]:
    wa, ha, ca, pa = read_png(a)
    wb, hb, cb, pb = read_png(b)
    if (wa, ha) != (wb, hb):
        return wa * ha, f"{a.name}: sizes differ ({wa}x{ha} against {wb}x{hb})"
    differ = beyond = biggest = 0
    x0, y0, x1, y1 = wa, ha, -1, -1
    diff = bytearray(wa * ha * 3) if diff_dir is not None else None
    for i in range(wa * ha):
        d = max(abs(pa[i * ca + c] - pb[i * cb + c]) for c in range(min(3, ca, cb)))
        if ca == 4 and cb == 4:
            d = max(d, abs(pa[i * 4 + 3] - pb[i * 4 + 3]))
        if d == 0:
            continue
        differ += 1
        biggest = max(biggest, d)
        if d > threshold:
            beyond += 1
            x, y = i % wa, i // wa
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if diff is not None:
            v = min(255, d * 16)
            diff[i * 3:i * 3 + 3] = bytes((255, 0, 0)) if d > threshold else bytes((v, v, v))
    if diff is not None:
        write_png(diff_dir / a.name, wa, ha, bytes(diff))
    where = f", beyond it in ({x0}, {y0})-({x1}, {y1})" if beyond else ""
    return beyond, (f"{a.name}: {differ} pixels differ ({100.0 * differ / (wa * ha):.2f}%), {beyond} by more than "
                    f"{threshold} ({100.0 * beyond / (wa * ha):.3f}%), the largest by {biggest}{where}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--threshold", type=int, default=8, help="a channel difference counted beyond (default 8)")
    ap.add_argument("--diff", type=Path, help="write each frame's differences here")
    args = ap.parse_args()
    if args.diff is not None:
        args.diff.mkdir(parents=True, exist_ok=True)
    frames = sorted(p for p in args.a.glob("*.png") if (args.b / p.name).exists())
    if not frames:
        print("no frames of the same name in both folders", file=sys.stderr)
        return 2
    worst = 0
    for frame in frames:
        beyond, line = compare(frame, args.b / frame.name, args.threshold, args.diff)
        worst = max(worst, beyond)
        print(line)
    return 1 if worst else 0


if __name__ == "__main__":
    sys.exit(main())
