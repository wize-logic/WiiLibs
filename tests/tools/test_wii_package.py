#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/wii_package.py: meta.xml well-formed and escaped, the zip's layout, the icon."""

import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import wii_package as wp  # noqa: E402

DOL = bytes(range(256)) * 20


def png_pixels(data):
    """decodes a PNG of 8-bit RGB, unfiltered rows, as wii_package writes it"""
    assert data.startswith(wp.PNG_SIGNATURE)
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        assert crc == zlib.crc32(kind + body) & 0xFFFFFFFF, kind
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = ihdr
    assert (depth, colour, interlace) == (8, 2, 0)
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    assert len(raw) == stride * height
    rows = []
    for y in range(height):
        assert raw[y * stride] == 0
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return width, height, rows


class MetaXml(unittest.TestCase):
    def parse(self, **kw):
        return ET.fromstring(wp.meta_xml(**kw))

    def test_fields(self):
        root = self.parse(name="App", coder="Me", version="1.2", short_description="short",
                          long_description="long\nlines", release_date="20260102030405", ahb_access=True)
        self.assertEqual("app", root.tag)
        self.assertEqual("1", root.get("version"))
        self.assertEqual("App", root.findtext("name"))
        self.assertEqual("Me", root.findtext("coder"))
        self.assertEqual("1.2", root.findtext("version"))
        self.assertEqual("20260102030405", root.findtext("release_date"))
        self.assertEqual("short", root.findtext("short_description"))
        self.assertEqual("long\nlines", root.findtext("long_description"))
        self.assertIsNotNone(root.find("ahb_access"))

    def test_escaped(self):
        nasty = 'A & B <c> "d" \'e\' ]]> &amp;'
        root = self.parse(name=nasty, coder=nasty, long_description=nasty, release_date="20260101000000")
        self.assertEqual(nasty, root.findtext("name"))
        self.assertEqual(nasty, root.findtext("long_description"))
        self.assertNotIn("<c>", wp.meta_xml(nasty))

    def test_control_characters_dropped(self):
        root = self.parse(name="a\x00b\x1fc\x08d\te")
        self.assertEqual("abcd\te", root.findtext("name"))

    def test_unicode(self):
        root = self.parse(name="Pok\u00e9 \u30b2\u30fc\u30e0")
        self.assertEqual("Pok\u00e9 \u30b2\u30fc\u30e0", root.findtext("name"))

    def test_optional_fields_omitted(self):
        root = self.parse(name="App")
        self.assertIsNone(root.find("coder"))
        self.assertIsNone(root.find("short_description"))
        self.assertIsNone(root.find("ahb_access"))
        self.assertIsNotNone(root.find("release_date"))


class Icon(unittest.TestCase):
    def test_size_and_decodes(self):
        for lines in (["A"], ["OPEN", "WII"], ["A VERY LONG APPLICATION NAME THAT NEEDS SHRINKING"], ["lower case ok"]):
            data = wp.icon_png(lines)
            self.assertEqual((128, 48), wp.png_size(data))
            width, height, rows = png_pixels(data)
            self.assertEqual((128, 48), (width, height))
            self.assertEqual(48, len(rows))

    def test_text_is_drawn(self):
        _, _, rows = png_pixels(wp.icon_png(["WII"]))
        self.assertIn(bytes((220, 230, 255)), b"".join(rows))
        blank = png_pixels(wp.icon_png([" "]))[2]
        self.assertNotIn(bytes((220, 230, 255)), b"".join(blank))

    def test_deterministic(self):
        self.assertEqual(wp.icon_png(["X", "Y"]), wp.icon_png(["X", "Y"]))

    def test_png_size_rejects_other_data(self):
        self.assertIsNone(wp.png_size(b"not a png"))
        self.assertIsNone(wp.png_size(b""))


class Package(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.dol = self.dir / "in.dol"
        self.dol.write_bytes(DOL)

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, **kw):
        out = self.dir / "out.zip"
        kw.setdefault("release_date", "20260101000000")
        members = wp.build_package(self.dol, out, **kw)
        return out, members

    def test_layout(self):
        out, members = self.build(name="My Game", coder="Me", version="1")
        with zipfile.ZipFile(out) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual(["apps/my-game/boot.dol", "apps/my-game/meta.xml", "apps/my-game/icon.png"],
                             z.namelist())
            self.assertEqual(members, z.namelist())
            self.assertEqual(DOL, z.read("apps/my-game/boot.dol"))
            self.assertEqual("My Game", ET.fromstring(z.read("apps/my-game/meta.xml")).findtext("name"))
            self.assertEqual((128, 48), wp.png_size(z.read("apps/my-game/icon.png")))
            for info in z.infolist():
                self.assertEqual((2026, 1, 1, 0, 0, 0), info.date_time)

    def test_app_dir(self):
        out, _ = self.build(name="X", app_dir="custom/dir")
        self.assertIn("apps/custom/dir/boot.dol", zipfile.ZipFile(out).namelist())
        for bad in ("..", "a/../b", "/", "", "C:x"):
            if bad == "":
                continue  # empty falls back to the slug
            with self.assertRaises(ValueError, msg=bad):
                self.build(name="X", app_dir=bad)

    def test_given_icon(self):
        icon = self.dir / "icon.png"
        icon.write_bytes(wp.icon_png(["MINE"]))
        out, _ = self.build(name="X", icon=icon)
        self.assertEqual(icon.read_bytes(), zipfile.ZipFile(out).read("apps/x/icon.png"))

    def test_wrong_icon_refused(self):
        icon = self.dir / "icon.png"
        icon.write_bytes(wp.icon_png(["MINE"], size=(64, 48)))
        with self.assertRaises(ValueError):
            self.build(name="X", icon=icon)
        icon.write_bytes(b"junk")
        with self.assertRaises(ValueError):
            self.build(name="X", icon=icon)

    def test_extras(self):
        (self.dir / "data").mkdir()
        (self.dir / "data" / "a.txt").write_text("a")
        (self.dir / "data" / "sub").mkdir()
        (self.dir / "data" / "sub" / "b.txt").write_text("b")
        (self.dir / "data" / ".hidden").write_text("h")
        (self.dir / "one.cfg").write_text("one")
        out, _ = self.build(name="X", extras=[(self.dir / "data", "stuff"), (self.dir / "one.cfg", "one.cfg")])
        names = zipfile.ZipFile(out).namelist()
        for want in ("apps/x/stuff/a.txt", "apps/x/stuff/sub/b.txt", "apps/x/one.cfg"):
            self.assertIn(want, names)
        self.assertFalse([n for n in names if ".hidden" in n])
        with self.assertRaises(ValueError):
            self.build(name="X", extras=[(self.dir / "one.cfg", "../escape")])
        with self.assertRaises(FileNotFoundError):
            self.build(name="X", extras=[(self.dir / "missing", "m")])

    def test_parse_extra(self):
        self.assertEqual((Path("a/b"), "b"), wp.parse_extra("a/b"))
        self.assertEqual((Path("a/b"), "c/d"), wp.parse_extra("a/b:c/d"))
        self.assertEqual((Path("C:\\x\\y"), "y"), wp.parse_extra("C:\\x\\y"))
        self.assertEqual((Path("C:\\x"), "z"), wp.parse_extra("C:\\x:z"))

    def test_reproducible(self):
        a, _ = self.build(name="X", coder="c")
        first = a.read_bytes()
        b, _ = self.build(name="X", coder="c")
        self.assertEqual(first, b.read_bytes())

    def test_missing_dol(self):
        self.dol.unlink()
        with self.assertRaises(FileNotFoundError):
            self.build(name="X")


class CommandLine(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "wii_package.py"), *args],
                              capture_output=True, text=True)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "g.dol").write_bytes(DOL)
            (tmp / "long.txt").write_text("from a file & more\n")
            out = tmp / "sub" / "g.zip"
            done = self.run_tool("--dol", str(tmp / "g.dol"), "--output", str(out), "--name", "Cmd",
                                 "--long-description-file", str(tmp / "long.txt"), "--icon-text", "ONE", "TWO",
                                 "--extra", f"{tmp / 'long.txt'}:notes.txt", "--release-date", "20260303030303")
            self.assertEqual(0, done.returncode, done.stderr)
            with zipfile.ZipFile(out) as z:
                self.assertEqual("from a file & more",
                                 ET.fromstring(z.read("apps/cmd/meta.xml")).findtext("long_description"))
                self.assertIn("apps/cmd/notes.txt", z.namelist())

    def test_errors(self):
        done = self.run_tool("--dol", "/nonexistent.dol", "--output", "/tmp/never.zip", "--name", "X")
        self.assertNotEqual(0, done.returncode)
        self.assertIn("no /nonexistent.dol", done.stderr)
        done = self.run_tool("--dol", "a", "--output", "b", "--name", "X", "--icon", "i", "--icon-text", "T")
        self.assertNotEqual(0, done.returncode)


if __name__ == "__main__":
    unittest.main()
