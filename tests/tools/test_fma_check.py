#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/fma_check.py: which mnemonics count as fused, the objdump listing parser
(objects, archives, sections), the filters, the command line (exit statuses, the stamp, a response
file) over a stand-in objdump that prints canned listings, and, where a PowerPC Linux compiler and
binutils are installed, real objects compiled with and without contraction (and assembled paired
singles)."""

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import fma_check  # noqa: E402

FMA = Path(fma_check.__file__)

HEADER = "{name}:     file format elf32-powerpc\n\n\nDisassembly of section .text:\n"

# an object of three functions: `fused` has two fused multiply-adds, `clean` has none (a multiply
# and an add apart, a paired-single multiply), `scaled` has one (a paired single)
OBJECT = HEADER.format(name="game.o") + """
00000000 <fused>:
   0:\tfmadd   f1,f1,f2,f3
   4:\tfmuls   f0,f1,f2
   8:\tfnmsubs f1,f1,f2,f3
   c:\tblr

00000010 <clean>:
  10:\tfmul    f1,f1,f2
  14:\tfadd    f1,f1,f3
  18:\tps_mul  f1,f2,f3
  1c:\tps_sum0 f1,f2,f3,f4
  20:\tblr

00000030 <scaled>:
  30:\tps_madds0. f1,f2,f3,f4
  34:\tblr
"""

CLEAN = HEADER.format(name="clean.o") + """
00000000 <only>:
   0:\tfmul    f1,f1,f2
   4:\tblr
"""

# a library: one member with a fused multiply-add, one without
ARCHIVE = """In archive libm.a:

""" + HEADER.format(name="e_pow.o") + """
00000000 <pow>:
   0:\tfmsub   f1,f1,f2,f3
   4:\tblr

""" + HEADER.format(name="e_sqrt.o") + """
00000000 <sqrt>:
   0:\tfsqrt   f1,f1
   4:\tblr
"""

# code before any symbol (a linked file's start-up) is named for its section
NO_SYMBOL = HEADER.format(name="raw.o") + "   0:\tfnmadd  f1,f1,f2,f3\n"

X86 = "x86.o:     file format elf32-i386\n\n\nDisassembly of section .text:\n\n00000000 <f>:\n   0:\tret\n"


class MnemonicTests(unittest.TestCase):
    def test_every_fused_form_is_found(self):
        fused = ["fmadd f1,f2,f3,f4", "fmadd. f1,f2,f3,f4", "fmadds f1,f2,f3,f4", "fmsub f1,f2,f3,f4",
                 "fmsubs. f1,f2,f3,f4", "fnmadd f1,f2,f3,f4", "fnmadds f1,f2,f3,f4", "fnmsub f1,f2,f3,f4",
                 "fnmsubs f1,f2,f3,f4", "ps_madd f1,f2,f3,f4", "ps_msub. f1,f2,f3,f4", "ps_nmadd f1,f2,f3,f4",
                 "ps_nmsub f1,f2,f3,f4", "ps_madds0 f1,f2,f3,f4", "ps_madds1. f1,f2,f3,f4", "vmaddfp v1,v2,v3,v4",
                 "vnmsubfp v1,v2,v3,v4", "xsmaddadp vs1,vs2,vs3", "xsmaddmdp vs1,vs2,vs3", "xsnmsubasp vs1,vs2,vs3",
                 "xvmaddadp vs1,vs2,vs3", "xvnmsubmsp vs1,vs2,vs3"]
        for text in fused:
            self.assertTrue(fma_check.is_fused(text), text)

    def test_other_instructions_are_not(self):
        other = ["fmul f1,f2,f3", "fmuls f1,f2,f3", "fadd f1,f2,f3", "fsub f1,f2,f3", "fmr f1,f2", "fnabs f1,f2",
                 "fneg f1,f2", "fsel f1,f2,f3,f4", "ps_mul f1,f2,f3", "ps_muls0 f1,f2,f3", "ps_sum0 f1,f2,f3,f4",
                 "ps_add f1,f2,f3", "ps_merge00 f1,f2,f3", "vaddfp v1,v2,v3", "xsadddp vs1,vs2,vs3", "mullw r3,r4,r5",
                 "madd r1,r2,r3", "blr", "", "fmaddx f1,f2,f3,f4"]
        for text in other:
            self.assertFalse(fma_check.is_fused(text), text)


class ParseTests(unittest.TestCase):
    def test_functions_are_reported_with_their_instructions(self):
        found = fma_check.parse_listing(OBJECT)
        self.assertEqual([(f.unit, f.function, len(f.instructions)) for f in found],
                         [("game.o", "fused", 2), ("game.o", "scaled", 1)])
        self.assertEqual(found[0].instructions, [("0", "fmadd   f1,f1,f2,f3"), ("8", "fnmsubs f1,f1,f2,f3")])
        self.assertEqual(found[0].mnemonics, ["fmadd", "fnmsubs"])
        self.assertEqual(found[1].mnemonics, ["ps_madds0"])

    def test_a_clean_object_has_no_findings(self):
        self.assertEqual(fma_check.parse_listing(CLEAN), [])

    def test_archive_members_are_named_in_the_archive(self):
        found = fma_check.parse_listing(ARCHIVE)
        self.assertEqual([(f.unit, f.function) for f in found], [("libm.a(e_pow.o)", "pow")])

    def test_several_objects_in_one_listing(self):
        found = fma_check.parse_listing(CLEAN + "\n" + OBJECT)
        self.assertEqual([f.unit for f in found], ["game.o", "game.o"])

    def test_code_before_any_symbol_is_named_for_its_section(self):
        found = fma_check.parse_listing(NO_SYMBOL)
        self.assertEqual([(f.unit, f.function) for f in found], [("raw.o", "<.text>")])

    def test_an_object_that_is_not_powerpc_is_an_error(self):
        with self.assertRaises(fma_check.CheckError) as raised:
            fma_check.parse_listing(X86)
        self.assertIn("elf32-i386", str(raised.exception))
        self.assertEqual(fma_check.parse_listing(HEADER.format(name="le.o").replace("elf32-powerpc", "elf32-powerpcle")), [])


class FilterTests(unittest.TestCase):
    def test_object_patterns_match_the_path_the_base_name_and_the_archive_and_member(self):
        names = fma_check.unit_names("build/obj/game.o")
        self.assertIn("build/obj/game.o", names)
        self.assertIn("game.o", names)
        names = fma_check.unit_names("/opt/lib/libm.a(e_pow.o)")
        self.assertEqual(sorted(names), sorted(["/opt/lib/libm.a(e_pow.o)", "e_pow.o", "/opt/lib/libm.a", "libm.a"]))

    def test_exclude_and_include(self):
        keep = lambda unit, function=None, **kw: fma_check.wanted(  # noqa: E731
            unit, function, kw.get("include", ()), kw.get("exclude", ()), kw.get("include_function", ()),
            kw.get("exclude_function", ()))
        self.assertTrue(keep("a/b.o"))
        self.assertFalse(keep("a/b.o", exclude=["b.o"]))
        self.assertFalse(keep("a/b.o", exclude=["a/*"]))
        self.assertTrue(keep("a/b.o", exclude=["c.o"]))
        self.assertTrue(keep("a/b.o", include=["*.o"]))
        self.assertFalse(keep("a/b.o", include=["c*"]))
        self.assertFalse(keep("a/b.o", include=["*.o"], exclude=["b.o"]))
        self.assertFalse(keep("libm.a(e_pow.o)", exclude=["libm.a"]))
        self.assertFalse(keep("libm.a(e_pow.o)", exclude=["e_pow.o"]))
        self.assertTrue(keep("libm.a(e_pow.o)", exclude=["e_sqrt.o"]))
        self.assertFalse(keep("a/b.o", "wl_x", exclude_function=["wl_*"]))
        self.assertTrue(keep("a/b.o", "game_x", exclude_function=["wl_*"]))
        self.assertTrue(keep("a/b.o", "wl_x", include_function=["wl_*"]))
        self.assertFalse(keep("a/b.o", "game_x", include_function=["wl_*"]))


STAND_IN = """#!{python}
import sys
args = sys.argv[1:]
if args[:2] != ["-d", "--no-show-raw-insn"]:
    sys.exit("stand-in objdump: unexpected arguments " + " ".join(args))
args = args[2:]
mode = None
if args[:1] == ["-M"]:
    mode, args = args[1], args[2:]
open(sys.argv[0] + ".log", "a").write(repr((mode, args)) + "\\n")
for path in args:
    text = open(path).read()
    if text.startswith("BROKEN"):
        sys.stderr.write(path + ": file format not recognized\\n")
        sys.exit(1)
    sys.stdout.write(text)
"""


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.dir = Path(self.directory.name)
        self.objdump = self.dir / "objdump"
        self.objdump.write_text(STAND_IN.format(python=sys.executable))
        self.objdump.chmod(self.objdump.stat().st_mode | stat.S_IXUSR)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, name, text):
        path = self.dir / name
        path.write_text(text)
        return str(path)

    def run_tool(self, *args, **kwargs):
        return subprocess.run([sys.executable, str(FMA), "--objdump", str(self.objdump), *args],
                              capture_output=True, text=True, **kwargs)

    def test_a_clean_object_passes_and_touches_the_stamp(self):
        stamp = self.dir / "ok.stamp"
        result = self.run_tool(self.write("clean.o", CLEAN), "--stamp", str(stamp))
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertIn("no fused multiply-adds in 1 file", result.stdout)
        self.assertTrue(stamp.exists())

    def test_a_fused_multiply_add_fails_lists_the_function_and_leaves_no_stamp(self):
        stamp = self.dir / "bad.stamp"
        result = self.run_tool(self.write("game.o", OBJECT), self.write("clean.o", CLEAN), "--stamp", str(stamp))
        self.assertEqual(result.returncode, 1)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].endswith("game.o: fused: 2 fused multiply-adds (fmadd, fnmsubs)"), lines[0])
        self.assertTrue(lines[1].endswith("game.o: scaled: 1 fused multiply-add (ps_madds0)"), lines[1])
        self.assertIn("3 fused multiply-add(s) in 2 function(s)", result.stderr)
        self.assertFalse(stamp.exists())

    def test_instructions_are_listed_with_v(self):
        result = self.run_tool("-v", "-q", self.write("game.o", OBJECT))
        self.assertEqual(result.returncode, 1)
        self.assertIn("    8: fnmsubs f1,f1,f2,f3", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_quiet_prints_nothing_when_clean(self):
        result = self.run_tool("-q", self.write("clean.o", CLEAN))
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "", ""))

    def test_filters_clear_what_they_exclude(self):
        game = self.write("game.o", OBJECT)
        self.assertEqual(self.run_tool("-q", "--exclude", "game.o", game).returncode, 0)
        self.assertEqual(self.run_tool("-q", "--include", "other*", game).returncode, 0)
        self.assertEqual(self.run_tool("-q", "--exclude-function", "fused", game).returncode, 1)
        result = self.run_tool("-q", "--exclude-function", "fused", "--exclude-function", "sc*", game)
        self.assertEqual(result.returncode, 0)
        result = self.run_tool("-q", "--include-function", "scaled", game)
        self.assertEqual((result.returncode, len(result.stdout.splitlines())), (1, 1))

    def test_an_excluded_object_is_not_read(self):
        # a file objdump would fail on is not given to it when the filters leave it out
        broken = self.write("broken.o", "BROKEN")
        self.assertEqual(self.run_tool("-q", "--exclude", "broken.o", broken, self.write("clean.o", CLEAN)).returncode, 0)
        self.assertEqual(self.run_tool("-q", broken).returncode, 2)

    def test_archives_are_read_and_filtered_by_member(self):
        archive = self.write("libm.a", ARCHIVE)
        result = self.run_tool("-q", archive)
        self.assertEqual(result.returncode, 1)
        self.assertIn("libm.a(e_pow.o): pow:", result.stdout)
        self.assertEqual(self.run_tool("-q", "--exclude", "e_pow.o", archive).returncode, 0)
        self.assertEqual(self.run_tool("-q", "--exclude", "*libm.a", archive).returncode, 0)

    def test_a_response_file_names_the_objects_quoted_as_a_shell_does(self):
        spaced = self.write("my game.o", OBJECT)
        response = self.write("objects.rsp", f"'{spaced}'\n{self.write('clean.o', CLEAN)}\n")
        result = self.run_tool(f"@{response}", "-q")
        self.assertEqual(result.returncode, 1)
        self.assertIn("game.o: fused:", result.stdout)
        self.assertEqual(Path(str(self.objdump) + ".log").read_text().splitlines(),
                         [repr(("gekko", [spaced, str(self.dir / "clean.o")]))])

    def test_the_check_that_cannot_be_made_is_not_a_pass(self):
        stamp = self.dir / "never.stamp"
        for args in (["--stamp", str(stamp), str(self.dir / "missing.o")],
                     ["--stamp", str(stamp), self.write("broken.o", "BROKEN")],
                     ["--stamp", str(stamp), self.write("x86.o", X86)],
                     ["--stamp", str(stamp), str(self.dir)],
                     ["--stamp", str(stamp), "@" + self.write("empty.rsp", "")]):
            result = self.run_tool(*args)
            self.assertEqual(result.returncode, 2, (args, result.stderr))
            self.assertIn("fma_check.py:", result.stderr)
            self.assertFalse(stamp.exists())
        result = subprocess.run([sys.executable, str(FMA), "--objdump", str(self.dir / "no_such_objdump"),
                                 self.write("clean.o", CLEAN)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot run", result.stderr)

    def test_the_gekko_is_the_default_disassembler_mode_and_can_be_changed(self):
        clean = self.write("clean.o", CLEAN)
        self.run_tool(clean)
        self.run_tool("--disassembler-options", "altivec,vsx", clean)
        self.run_tool("--disassembler-options", "", clean)
        log = Path(str(self.objdump) + ".log").read_text().splitlines()
        self.assertEqual(log, [repr(("gekko", [clean])), repr(("altivec,vsx", [clean])), repr((None, [clean]))])

    def test_many_objects_are_read_in_batches(self):
        paths = [self.write(f"o{i}.o", CLEAN) for i in range(fma_check.BATCH + 5)]
        self.assertEqual(self.run_tool("-q", *paths).returncode, 0)
        batches = Path(str(self.objdump) + ".log").read_text().splitlines()
        self.assertEqual(len(batches), 2)


class ObjdumpSearchTests(unittest.TestCase):
    def test_the_order_is_the_argument_then_the_environment_then_devkitppc_then_the_path(self):
        with tempfile.TemporaryDirectory() as directory:
            bin_dir = Path(directory) / "bin"
            kit = Path(directory) / "kit" / "devkitPPC" / "bin"
            for folder, name in ((bin_dir, "powerpc-linux-gnu-objdump"), (kit, "powerpc-eabi-objdump")):
                folder.mkdir(parents=True)
                (folder / name).write_text("#!/bin/sh\n")
                (folder / name).chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": str(bin_dir)}):
                self.assertEqual(fma_check.find_objdump(None, {}), str(bin_dir / "powerpc-linux-gnu-objdump"))
                self.assertEqual(fma_check.find_objdump(None, {"DEVKITPRO": str(Path(directory) / "kit")}),
                                 str(kit / "powerpc-eabi-objdump"))
                self.assertEqual(fma_check.find_objdump(None, {"OBJDUMP": "mine"}), "mine")
                self.assertEqual(fma_check.find_objdump("given", {"OBJDUMP": "mine"}), "given")
            with mock.patch.dict(os.environ, {"PATH": directory}):
                with self.assertRaises(fma_check.CheckError):
                    fma_check.find_objdump(None, {})


FAST_C = """\
double madd(double a, double b, double c) { return a * b + c; }
float msubs(float a, float b, float c) { return a * b - c; }
double plain(double a, double b) { return a + b; }
"""

PAIRS_S = """\
\t.text
\t.globl\tpair
pair:
\tps_nmadd\t1,2,3,4
\tps_nmsub\t1,2,3,4
\tps_madds0\t1,2,3,4
\tblr
\t.globl\tmul
mul:
\tps_mul\t1,2,3
\tps_sum0\t1,2,3,4
\tblr
"""


def powerpc_objdump():
    return shutil.which("powerpc-eabi-objdump") or shutil.which("powerpc-linux-gnu-objdump")


@unittest.skipUnless(shutil.which("powerpc-linux-gnu-gcc") and powerpc_objdump(),
                     "needs powerpc-linux-gnu-gcc and a PowerPC objdump")
class RealObjectTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.dir = Path(self.directory.name)
        (self.dir / "fast.c").write_text(FAST_C)

    def tearDown(self):
        self.directory.cleanup()

    def compile(self, name, contract):
        out = self.dir / name
        subprocess.run(["powerpc-linux-gnu-gcc", "-O2", "-mcpu=750", f"-ffp-contract={contract}", "-c",
                        str(self.dir / "fast.c"), "-o", str(out)], check=True)
        return out

    def test_contracted_code_is_caught_and_uncontracted_code_is_not(self):
        fast, off = self.compile("fast.o", "fast"), self.compile("off.o", "off")
        found = fma_check.scan(powerpc_objdump(), [str(fast), str(off)])
        self.assertEqual([(f.unit, f.function) for f in found], [(str(fast), "madd"), (str(fast), "msubs")])
        self.assertEqual(found[0].mnemonics, ["fmadd"])
        self.assertEqual(found[1].mnemonics, ["fmsubs"])
        self.assertEqual(fma_check.scan(powerpc_objdump(), [str(off)]), [])

    def test_a_contracted_member_of_an_archive_and_a_linked_file(self):
        fast = self.compile("fast.o", "fast")
        archive = self.dir / "libfast.a"
        subprocess.run(["powerpc-linux-gnu-ar", "rc", str(archive), str(fast)], check=True)
        found = fma_check.scan(powerpc_objdump(), [str(archive)])
        self.assertEqual([f.unit for f in found], [f"{archive}(fast.o)"] * 2)
        self.assertEqual(fma_check.scan(powerpc_objdump(), [str(archive)], exclude=["fast.o"]), [])
        program = self.dir / "program"
        (self.dir / "main.c").write_text("int main(void) { return 0; }\n")
        subprocess.run(["powerpc-linux-gnu-gcc", "-O2", "-mcpu=750", "-ffp-contract=fast", "-static",
                        str(self.dir / "fast.c"), str(self.dir / "main.c"), "-o", str(program)], check=True)
        found = fma_check.scan(powerpc_objdump(), [str(program)], include_function=["madd", "msubs"])
        self.assertEqual(sorted(f.function for f in found), ["madd", "msubs"])

    def test_paired_singles_that_a_default_objdump_misreads_are_caught(self):
        assembler = shutil.which("powerpc-eabi-as") or shutil.which("powerpc-linux-gnu-as")
        source, out = self.dir / "pairs.s", self.dir / "pairs.o"
        source.write_text(PAIRS_S)
        if subprocess.run([assembler, "-mgekko", str(source), "-o", str(out)], capture_output=True).returncode:
            self.skipTest("the assembler does not know the Gekko")
        try:
            found = fma_check.scan(powerpc_objdump(), [str(out)])
        except fma_check.CheckError:
            self.skipTest("the objdump does not take -M gekko")
        self.assertEqual([(f.function, f.mnemonics) for f in found],
                         [("pair", ["ps_madds0", "ps_nmadd", "ps_nmsub"])])

    def test_the_command_line_on_a_real_object(self):
        fast, off = self.compile("fast.o", "fast"), self.compile("off.o", "off")
        run = lambda *objects: subprocess.run(  # noqa: E731
            [sys.executable, str(FMA), "--objdump", powerpc_objdump(), "-q", *map(str, objects)],
            capture_output=True, text=True)
        self.assertEqual(run(off).returncode, 0)
        result = run(off, fast)
        self.assertEqual(result.returncode, 1)
        self.assertIn("fast.o: madd: 1 fused multiply-add (fmadd)", result.stdout)


if __name__ == "__main__":
    unittest.main()
