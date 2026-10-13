#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/xbox_recomp.py, the driver of the patched xboxrecomp pipeline.

On the synthetic XBE upstream's mkxbe.py writes: the stages run and write what a build needs (the
fixed set of translation units, the report), a second run reruns nothing, a title config reaches
the stages it is for (and only those rerun), the runtime header choice is passed on, a bad config
is refused, and the patched pipeline is keyed on the vendored files and the patches. Needs capstone.
"""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
WL = HERE.parent.parent
DRIVER = WL / "tools" / "xbox_recomp.py"
MKXBE = WL / "third_party" / "xboxrecomp" / "tools" / "conformance" / "mkxbe.py"
sys.path.insert(0, str(WL / "tools"))
import xbox_recomp  # noqa: E402


def run(*args, check=True):
    r = subprocess.run([sys.executable, str(DRIVER), *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise AssertionError(f"{args} exited {r.returncode}:\n{r.stdout}\n{r.stderr}")
    return r


class DriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = Path(tempfile.mkdtemp(prefix="wl-xbox-recomp-test-"))
        cls.xbe = cls.td / "synth.xbe"
        subprocess.run([sys.executable, str(MKXBE), str(cls.xbe)], check=True, capture_output=True)
        cls.out = cls.td / "out"
        cls.first = run("run", str(cls.xbe), "-o", str(cls.out), "--chunks", "3")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.td, ignore_errors=True)

    def test_outputs(self):
        tu = sorted(p.name for p in (self.out / "tu").iterdir())
        self.assertEqual(tu, ["recomp_tu_00.c", "recomp_tu_01.c", "recomp_tu_02.c",
                              "recomp_tu_dispatch.c", "recomp_tu_patches.c", "recomp_tu_stubs.c"])
        self.assertIn("wl_xb_title_patch_count = 0u", (self.out / "tu" / "recomp_tu_patches.c").read_text())
        self.assertIn('#include "../gen/recomp_0000.c"', (self.out / "tu" / "recomp_tu_00.c").read_text())
        self.assertIn("recomp_tu_01_empty", (self.out / "tu" / "recomp_tu_01.c").read_text())
        rep = json.loads((self.out / "xbox_recomp.json").read_text())
        self.assertEqual(rep["functions_detected"], 5)
        self.assertEqual(rep["functions_translated"], 5)
        self.assertEqual(rep["unimplemented_instructions"], 0)
        gen = self.out / "gen"
        self.assertFalse((gen / "recomp_types.h").exists(), "the build supplies WiiLibs' header")
        self.assertIn("void sub_00011020(void)", (gen / "recomp_0000.c").read_text())
        # patched: the shift count is masked (patch 0001)
        self.assertIn("& 31u", (gen / "recomp_0000.c").read_text())
        self.assertTrue((self.out / "pipeline" / ".wl_pipeline.json").exists())

    def test_second_run_reruns_nothing(self):
        r = run("run", str(self.xbe), "-o", str(self.out), "--chunks", "3")
        for stage in ("parse", "disasm", "func_id", "abi", "recomp"):
            self.assertIn(f"{stage}: up to date", r.stderr)

    def test_config_reaches_recomp_only(self):
        out = self.td / "cfg"
        run("run", str(self.xbe), "-o", str(out), "--chunks", "2")
        cfg = self.td / "title.json"
        cfg.write_text(json.dumps({"name": "Synth", "manual_functions": {"0x00011040": "my_bits"}}))
        r = run("run", str(self.xbe), "-o", str(out), "--chunks", "2", "--config", str(cfg))
        self.assertIn("disasm: up to date", r.stderr)
        self.assertIn("recomp: running", r.stderr)
        code = (out / "gen" / "recomp_0000.c").read_text()
        self.assertNotIn("void my_bits(void)\n{", code)          # written by hand, not generated
        self.assertIn("my_bits", (out / "gen" / "recomp_funcs.h").read_text())
        self.assertIn("Synth", code)

    def xbe_bytes_at(self, va, n):
        d = self.xbe.read_bytes()
        u32 = lambda o: int.from_bytes(d[o:o + 4], "little")
        base, count, table = u32(0x104), u32(0x11C), u32(0x120) - u32(0x104)
        for i in range(count):
            o = table + i * 56
            sva, raw, rawsize = u32(o + 4), u32(o + 12), u32(o + 16)
            if sva <= va and va + n <= sva + rawsize:
                return d[raw + va - sva:raw + va - sva + n]
        raise AssertionError(f"0x{va:08X} not in the XBE")

    def test_patches_are_checked_applied_and_tabled(self):
        here = self.xbe_bytes_at(0x00011020, 2)
        cfg = self.td / "patch.json"
        cfg.write_text(json.dumps({"patches": [{"va": "0x00011020", "from": here.hex(), "to": here.hex(),
                                                "why": "a no-op patch"}]}))
        out = self.td / "patched"
        run("run", str(self.xbe), "-o", str(out), "--chunks", "2", "--config", str(cfg))
        self.assertTrue((out / "xbe" / "patched.xbe").is_file())
        table = (out / "tu" / "recomp_tu_patches.c").read_text()
        self.assertIn("0x00011020u, 2u", table)
        self.assertIn("wl_xb_title_patch_count = 1u", table)
        # bytes the XBE does not have: refused, naming what it has
        wrong = bytes(b ^ 0xFF for b in here)
        cfg.write_text(json.dumps({"patches": [{"va": "0x00011020", "from": wrong.hex(), "to": here.hex()}]}))
        r = run("run", str(self.xbe), "-o", str(out), "--chunks", "2", "--config", str(cfg), check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn(f"the XBE has {here.hex()}", r.stderr)

    def test_runtime_header_template(self):
        out = self.td / "tpl"
        run("run", str(self.xbe), "-o", str(out), "--runtime-header", "template")
        self.assertIn("MEMF_SET", (out / "gen" / "recomp_types.h").read_text())

    def test_bad_config_is_refused(self):
        cfg = self.td / "bad.json"
        cfg.write_text(json.dumps({"nmae": "typo"}))
        r = run("run", str(self.xbe), "-o", str(self.td / "bad"), "--config", str(cfg), check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown keys ['nmae']", r.stderr)
        r = run("run", str(self.td / "missing.xbe"), "-o", str(self.td / "bad"), check=False)
        self.assertEqual(r.returncode, 1)

    def test_pipeline_hash_follows_patches(self):
        patches = self.td / "patches"
        shutil.copytree(xbox_recomp.PATCHES, patches)
        h1 = xbox_recomp.pipeline_hash(patches=patches)
        self.assertEqual(h1, xbox_recomp.pipeline_hash())
        first = sorted(patches.glob("*.patch"))[0]
        first.write_text(first.read_text() + "\n")
        self.assertNotEqual(h1, xbox_recomp.pipeline_hash(patches=patches))
        self.assertEqual(xbox_recomp.check_pristine(), [], "the vendored copy must stay pristine")

    def test_a_bad_patch_fails_preparation(self):
        patches = self.td / "badpatches"
        patches.mkdir()
        (patches / "0001-bad.patch").write_text(
            "diff --git a/tools/__init__.py b/tools/__init__.py\n--- a/tools/__init__.py\n"
            "+++ b/tools/__init__.py\n@@ -1,1 +1,1 @@\n-this line is not there\n+x\n")
        with self.assertRaises(xbox_recomp.Failure) as e:
            xbox_recomp.prepare_pipeline(self.td / "badpipe", patches_dir=patches, quiet=True)
        self.assertIn("0001-bad.patch", str(e.exception))


if __name__ == "__main__":
    try:
        import capstone  # noqa: F401
    except ImportError:
        print("skipped: Python module capstone missing")
        sys.exit(77)
    unittest.main()
