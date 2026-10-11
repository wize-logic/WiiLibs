#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the zips tests/tools/CMakeLists.txt builds with wl_add_hbc_package.

usage: check_hbc_zip.py <file-based zip> <target-based zip>"""

import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
import wii_package as wp  # noqa: E402


def need(condition, message):
    if not condition:
        sys.exit(f"check_hbc_zip.py: {message}")


def main(file_zip, program_zip):
    with zipfile.ZipFile(file_zip) as z:
        need(z.testzip() is None, "corrupt zip")
        need(z.namelist() == ["apps/fixture-co/boot.dol", "apps/fixture-co/meta.xml", "apps/fixture-co/icon.png",
                              "apps/fixture-co/notes/extra.py"], f"layout {z.namelist()}")
        need(z.read("apps/fixture-co/boot.dol") == (HERE / "test_wii_package.py").read_bytes(), "boot.dol")
        need(z.read("apps/fixture-co/notes/extra.py") == (HERE / "test_wii_package.py").read_bytes(), "extra")
        need(wp.png_size(z.read("apps/fixture-co/icon.png")) == (128, 48), "icon size")
        meta = ET.fromstring(z.read("apps/fixture-co/meta.xml"))
        need(meta.findtext("name") == "Fixture & Co", "name")
        need(meta.findtext("coder") == "Me <me>", "coder")
        need(meta.findtext("version") == "1.0", "version")
        need(meta.findtext("short_description") == "short", "short description")
        need(meta.findtext("long_description") == "A long description\nof two lines & a symbol.", "long description")
        need(meta.findtext("release_date") == "20260101000000", "release date")
        need(meta.find("ahb_access") is not None, "ahb_access")
    with zipfile.ZipFile(program_zip) as z:
        need(z.namelist() == ["apps/program/boot.dol", "apps/program/meta.xml", "apps/program/icon.png"],
             f"layout {z.namelist()}")
        need(z.read("apps/program/boot.dol")[:4] == b"\x7fELF", "boot.dol is the target's file")
        need(z.read("apps/program/icon.png") == (HERE / "icon_128x48.png").read_bytes(), "given icon")
    print("ok")


if __name__ == "__main__":
    need(len(sys.argv) == 3, "two zips")
    main(*sys.argv[1:])
