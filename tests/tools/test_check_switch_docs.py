#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tests/tools/check_switch_docs.py on a copy of the tree it reads (wl/**/*.c, the replay,
wl/xboxrt/README.md, tools/xbox_recomp.py, the xboxrecomp patches' names and README): the tree as it
is passes; a draw-path switch added to nv2a_gx.c's second word (k_switches2, SWITCH2_*) or its first
without a README row fails, naming its file and variable, and passes once the row is written; two
switches of one word on one bit fail.
"""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CHECK = HERE / "check_switch_docs.py"
GX = Path("wl/xbox/nv2a_gx.c")
README = Path("wl/xboxrt/README.md")


class SwitchDocs(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="switch_docs_"))
        for source in (ROOT / "wl").rglob("*.c"):
            target = self.tmp / source.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        for name in ("tools/xbox_nv2a_replay.c", "tools/xbox_recomp.py", str(README),
                     "third_party/xboxrecomp/README.md"):
            (self.tmp / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, self.tmp / name)
        patches = self.tmp / "third_party/xboxrecomp/patches"
        patches.mkdir(parents=True)
        for patch in (ROOT / "third_party/xboxrecomp/patches").iterdir():
            (patches / patch.name).touch()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def check(self):
        p = subprocess.run([sys.executable, "-I", str(CHECK), str(self.tmp)], capture_output=True, text=True)
        return p.returncode, p.stdout

    def edit(self, path: Path, old: str, new: str, count: int = 1):
        text = (self.tmp / path).read_text()
        self.assertIn(old, text)
        (self.tmp / path).write_text(text.replace(old, new, count))

    def second_word_switch(self, bit: int):
        """a switch "zzsecond" in the second word, on `bit`, as a cut adds it"""
        text = (self.tmp / GX).read_text()
        enum = re.search(r"enum \{\n(\s*SWITCH2_NO_PSH_MEMO = 1u << 8,[^\n]*\n)", text)
        self.assertIsNotNone(enum, "nv2a_gx.c's second word (SWITCH2_NO_PSH_MEMO) not found")
        self.edit(GX, enum.group(1), enum.group(1) + f"    SWITCH2_ZZ_SECOND = 1u << {bit},\n")
        table = re.search(r"k_switches2\[\] = \{\n", text)
        self.assertIsNotNone(table, "nv2a_gx.c's k_switches2 not found")
        self.edit(GX, table.group(0), table.group(0) + '    {"zzsecond", "WL_XB_ZZSECOND", SWITCH2_ZZ_SECOND},\n')

    def off_switch_row(self, file: str, env: str):
        self.edit(README, "| `oldfetch` |", f"| `{file}` | `{env}` | wave 7 | a test's switch |\n| `oldfetch` |")

    def test_the_tree_passes(self):
        code, out = self.check()
        self.assertEqual(0, code, out)
        self.assertIn("0 problems", out)

    def test_a_second_word_switch_without_a_row_fails(self):
        self.second_word_switch(1)
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("undocumented switch file: zzsecond", out)
        self.assertIn("undocumented variable: WL_XB_ZZSECOND", out)
        self.off_switch_row("zzsecond", "WL_XB_ZZSECOND")
        code, out = self.check()
        self.assertEqual(0, code, out)

    def test_a_first_word_switch_without_a_row_fails(self):
        self.edit(GX, '    {"oldfetch", "WL_XB_OLDFETCH", SWITCH_OLD_FETCH},\n',
                  '    {"oldfetch", "WL_XB_OLDFETCH", SWITCH_OLD_FETCH},\n'
                  '    {"zzfirst", "WL_XB_ZZFIRST", SWITCH_OLD_FETCH},\n')
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("undocumented switch file: zzfirst", out)

    def test_a_row_of_a_switch_nothing_reads_fails(self):
        self.off_switch_row("zzgone", "WL_XB_ZZGONE")
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("row names a switch nothing reads: zzgone", out)

    def test_two_switches_of_one_word_on_one_bit_fail(self):
        # (bit 8 taken while SWITCH2_NO_PSH_MEMO holds it)
        self.second_word_switch(8)
        self.off_switch_row("zzsecond", "WL_XB_ZZSECOND")
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("switch bit 8 of SWITCH2_ taken twice: SWITCH2_NO_PSH_MEMO, SWITCH2_ZZ_SECOND", out)
        # (the first word's: oldfetch's bit 31 again)
        self.edit(GX, "SWITCH2_ZZ_SECOND = 1u << 8", "SWITCH2_ZZ_SECOND = 1u << 1")
        self.edit(GX, "    SWITCH_OLD_FETCH = 1u << 31,", "    SWITCH_OLD_FETCH = 1u << 31,\n    SWITCH_ZZ = 1u << 31,")
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("switch bit 31 of SWITCH_ taken twice: SWITCH_OLD_FETCH, SWITCH_ZZ", out)


if __name__ == "__main__":
    unittest.main()
