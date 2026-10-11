#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Packages a Wii DOL for the Homebrew Channel.

The zip holds apps/<app-dir>/ as the channel reads it:
  boot.dol   the program
  meta.xml   the name, coder, version and descriptions the channel shows
  icon.png   128x48: the file given with --icon, or one drawn here (no artwork is needed)
plus any --extra files. Unzipped onto the root of an SD card or USB drive it runs from the
channel. The zip is reproducible: the same inputs (and --release-date, or SOURCE_DATE_EPOCH)
give the same bytes.

usage: wii_package.py --dol game.dol --output game.zip --name "My Game" [--coder X]
       [--version 1.0] [--short-description S] [--long-description L | --long-description-file F]
       [--icon icon.png | --icon-text LINE [LINE]] [--extra SRC[:DEST]]... [--ahb-access]

--extra SRC[:DEST] adds a file, or a folder with everything under it, at DEST inside the app's
folder (DEST defaults to SRC's name). No game data or keys belong in a package you share.
"""

import argparse
import calendar
import os
import re
import struct
import sys
import time
import zipfile
import zlib
from pathlib import Path
from xml.sax.saxutils import escape

ICON_WIDTH, ICON_HEIGHT = 128, 48
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# 5x7 glyphs for the drawn icon's lettering (upper case; other letters are drawn as capitals)
_GLYPHS = {
    "A": "01110 10001 10001 11111 10001 10001 10001",
    "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110",
    "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 10000 11110 10000 10000 11111",
    "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111",
    "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "11111 00100 00100 00100 00100 00100 11111",
    "J": "00111 00010 00010 00010 00010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001",
    "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001",
    "N": "10001 11001 10101 10011 10001 10001 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110",
    "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101",
    "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110",
    "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110",
    "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 11011 10001",
    "X": "10001 10001 01010 00100 01010 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100",
    "Z": "11111 00001 00010 00100 01000 10000 11111",
    "0": "01110 10001 10011 10101 11001 10001 01110",
    "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111",
    "3": "11110 00001 00001 01110 00001 00001 11110",
    "4": "00010 00110 01010 10010 11111 00010 00010",
    "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110",
    "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110",
    "9": "01110 10001 10001 01111 00001 00010 01100",
    "-": "00000 00000 00000 11111 00000 00000 00000",
    ".": "00000 00000 00000 00000 00000 01100 01100",
    "!": "00100 00100 00100 00100 00100 00000 00100",
    "?": "01110 10001 00001 00010 00100 00000 00100",
    ":": "00000 01100 01100 00000 01100 01100 00000",
    "'": "00100 00100 01000 00000 00000 00000 00000",
    "+": "00000 00100 00100 11111 00100 00100 00000",
    "/": "00001 00001 00010 00100 01000 10000 10000",
    " ": "00000 00000 00000 00000 00000 00000 00000",
}
FONT = {key: value.split() for key, value in _GLYPHS.items()}
GLYPH_ADVANCE = 6  # 5 columns and a gap


def text_width(line, scale):
    """pixels a line of text takes at a scale (no trailing gap)"""
    return max(0, len(line) * GLYPH_ADVANCE - 1) * scale


def fit_scale(line, preferred, width=ICON_WIDTH - 8):
    """the largest scale up to `preferred` at which the line fits the icon with a margin"""
    scale = preferred
    while scale > 1 and text_width(line, scale) > width:
        scale -= 1
    return scale


def icon_png(lines, size=(ICON_WIDTH, ICON_HEIGHT)):
    """A dark blue gradient with one or two lines of lettering, as a PNG.

    One line is drawn large and centred; two lines are drawn large over smaller. Text that
    would not fit is shrunk, then clipped at the icon's edge."""
    width, height = size
    lines = [line.upper() for line in lines if line][:2] or [" "]
    pixels = [[(10, 20 + y * 40 // height, 40 + y * 90 // height) for _ in range(width)] for y in range(height)]

    def draw(line, top, scale, colour):
        left = (width - text_width(line, scale)) // 2
        for index, letter in enumerate(line):
            for row, bits in enumerate(FONT.get(letter, FONT["?"])):
                for column, bit in enumerate(bits):
                    if bit != "1":
                        continue
                    for dy in range(scale):
                        for dx in range(scale):
                            x = left + (index * GLYPH_ADVANCE + column) * scale + dx
                            y = top + row * scale + dy
                            if 0 <= x < width and 0 <= y < height:
                                pixels[y][x] = colour

    if len(lines) == 1:
        scale = fit_scale(lines[0], 3)
        draw(lines[0], (height - 7 * scale) // 2, scale, (220, 230, 255))
    else:
        top_scale = fit_scale(lines[0], 3)
        bottom_scale = fit_scale(lines[1], 2)
        draw(lines[0], 4, top_scale, (220, 230, 255))
        draw(lines[1], height - 4 - 7 * bottom_scale, bottom_scale, (120, 170, 255))
    raw = b"".join(b"\0" + bytes(c for pixel in row for c in pixel) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def png_size(data):
    """(width, height) of a PNG, or None when `data` is not one"""
    if len(data) < 33 or not data.startswith(PNG_SIGNATURE) or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def slug(name):
    """a folder name for an application name: lower case letters, digits and dashes"""
    text = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return text or "app"


def xml_text(value):
    """escapes text for an element, and drops the characters XML 1.0 cannot carry"""
    cleaned = re.sub("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]", "", value)
    return escape(cleaned)


def release_stamp(epoch=None):
    """YYYYMMDDHHMMSS in UTC, from an epoch (SOURCE_DATE_EPOCH when set, else now)"""
    if epoch is None:
        env = os.environ.get("SOURCE_DATE_EPOCH")
        epoch = int(env) if env and env.isdigit() else int(time.time())
    return time.strftime("%Y%m%d%H%M%S", time.gmtime(epoch))


def meta_xml(name, coder="", version="", short_description="", long_description="",
             release_date=None, ahb_access=False):
    """the Homebrew Channel's meta.xml. Every text is escaped; empty ones are left out
    except the name."""
    lines = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>', '<app version="1">',
             f"  <name>{xml_text(name)}</name>"]
    for tag, value in (("coder", coder), ("version", version),
                       ("release_date", release_date or release_stamp()),
                       ("short_description", short_description), ("long_description", long_description)):
        if value:
            lines.append(f"  <{tag}>{xml_text(value)}</{tag}>")
    if ahb_access:
        lines.append("  <ahb_access/>")
    lines.append("</app>")
    return "\n".join(lines) + "\n"


def parse_extra(spec):
    """'SRC' or 'SRC:DEST' -> (Path, DEST). A Windows drive letter ('C:\\x') is not a split."""
    source, destination = spec, None
    if ":" in spec:
        head, tail = spec.rsplit(":", 1)
        if len(head) != 1:
            source, destination = head, tail
    path = Path(source)
    return path, (destination if destination else re.split(r"[\\/]", source.rstrip("\\/"))[-1])


def safe_member(destination):
    """an archive path under the app's folder, refusing '..' and absolute paths"""
    parts = [p for p in destination.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or ".." in parts or re.match(r"^[A-Za-z]:", parts[0]):
        raise ValueError(f"bad destination {destination!r}")
    return "/".join(parts)


def _add(archive, member, data, stamp):
    info = zipfile.ZipInfo(member, date_time=stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    info.create_system = 3
    archive.writestr(info, data)


def build_package(dol, output, name, app_dir=None, coder="", version="", short_description="",
                  long_description="", icon=None, icon_text=None, extras=(), release_date=None,
                  ahb_access=False):
    """Writes the zip; returns the list of archive paths. `icon` is a PNG file (any size is
    refused but 128x48: the channel scales nothing), `icon_text` one or two lines for the
    drawn icon (the name when neither is given). `extras` is a list of (Path, dest)."""
    dol = Path(dol)
    if not dol.is_file():
        raise FileNotFoundError(f"no {dol}")
    folder = safe_member(app_dir or slug(name))
    if icon is not None:
        icon_data = Path(icon).read_bytes()
        if png_size(icon_data) != (ICON_WIDTH, ICON_HEIGHT):
            raise ValueError(f"{icon} is not a {ICON_WIDTH}x{ICON_HEIGHT} PNG")
    else:
        icon_data = icon_png(list(icon_text) if icon_text else [name])
    stamp_text = release_date or release_stamp()
    epoch = calendar.timegm(time.strptime(stamp_text, "%Y%m%d%H%M%S"))
    stamp = max(time.gmtime(epoch)[:6], (1980, 1, 1, 0, 0, 0))
    members = {
        f"apps/{folder}/boot.dol": dol.read_bytes(),
        f"apps/{folder}/meta.xml": meta_xml(name, coder, version, short_description, long_description,
                                            stamp_text, ahb_access).encode("utf-8"),
        f"apps/{folder}/icon.png": icon_data,
    }
    for source, destination in extras:
        base = safe_member(destination)
        if source.is_dir():
            for path in sorted(p for p in source.rglob("*") if p.is_file() and not any(
                    part.startswith(".") or part == "__pycache__" for part in p.relative_to(source).parts)):
                members[f"apps/{folder}/{base}/{path.relative_to(source).as_posix()}"] = path.read_bytes()
        elif source.is_file():
            members[f"apps/{folder}/{base}"] = source.read_bytes()
        else:
            raise FileNotFoundError(f"no {source}")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for member, data in members.items():
            _add(archive, member, data, stamp)
    return list(members)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dol", type=Path, required=True, help="the program (becomes boot.dol)")
    parser.add_argument("--output", type=Path, required=True, help="the zip to write")
    parser.add_argument("--name", required=True, help="the application's name")
    parser.add_argument("--app-dir", help="the folder under apps/ (default: the name, as a slug)")
    parser.add_argument("--coder", default="")
    parser.add_argument("--version", default="")
    parser.add_argument("--short-description", default="")
    parser.add_argument("--long-description", default="")
    parser.add_argument("--long-description-file", type=Path, help="the long description from a file")
    parser.add_argument("--icon", type=Path, help="a 128x48 PNG (default: drawn)")
    parser.add_argument("--icon-text", nargs="+", metavar="LINE", help="one or two lines for the drawn icon")
    parser.add_argument("--extra", action="append", default=[], metavar="SRC[:DEST]",
                        help="a file or folder to add inside the app's folder")
    parser.add_argument("--release-date", help="YYYYMMDDHHMMSS (default: SOURCE_DATE_EPOCH or now)")
    parser.add_argument("--ahb-access", action="store_true", help="ask the channel for AHBPROT access")
    args = parser.parse_args(argv)
    if args.icon and args.icon_text:
        parser.error("--icon and --icon-text are alternatives")
    if args.release_date and not re.fullmatch(r"\d{14}", args.release_date):
        parser.error("--release-date is YYYYMMDDHHMMSS")
    long_description = args.long_description
    if args.long_description_file:
        long_description = args.long_description_file.read_text(encoding="utf-8").strip()
    try:
        members = build_package(args.dol, args.output, args.name, args.app_dir, args.coder, args.version,
                                args.short_description, long_description, args.icon, args.icon_text,
                                [parse_extra(spec) for spec in args.extra], args.release_date, args.ahb_access)
    except (OSError, ValueError) as error:
        sys.exit(f"wii_package.py: {error}")
    print(f"{args.output}: {len(members)} files ({', '.join(Path(m).name for m in members[:3])}"
          f"{', ...' if len(members) > 3 else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
