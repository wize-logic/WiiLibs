#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/link_stubs.py: the names it takes from a linker's errors, the C file it writes,
and whole links done with the host's C compiler standing in for the Wii's: a complete program
(nothing stubbed), one that calls functions it lacks (stubbed, and stopping with the name when
run), a data symbol, another stop function, a failure that is not an undefined symbol, and a
response file. With the paths of the built wl_platform and wl_base libraries and the source
folder as arguments, also a program linked against the stop screen of wl/platform/stop.h.

usage: test_link_stubs.py [LIBWL_PLATFORM LIBWL_BASE SOURCE_DIR]
"""

import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import link_stubs  # noqa: E402

LIBRARIES = sys.argv[1:4] if len(sys.argv) >= 4 else None
del sys.argv[1:]  # (unittest's own arguments)

CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")

GNU_LD = """\
/usr/bin/ld: main.o: in function `main':
main.c:(.text+0x5): undefined reference to `D3DDevice_Clear'
/usr/bin/ld: main.c:(.text+0xa): undefined reference to `D3DDevice_Clear'
/usr/bin/ld: main.c:(.text+0xf): undefined reference to `XGetDevices'
collect2: error: ld returned 1 exit status
"""

TYPOGRAPHIC = "main.c:(.text+0x5): undefined reference to ‘Beta’\n"

LLD = """\
ld.lld: error: undefined symbol: gamma
>>> referenced by main.c
>>>               main.o:(main)
"""

# a program that stands in for the stop function: the name on stdout, status 3
STOP = """\
#include <stdio.h>
#include <stdlib.h>
void stop_with(const char *name) { printf("stopped by %s\\n", name); exit(3); }
void wl_platform_missing_symbol(const char *name) { printf("missing %s\\n", name); exit(4); }
"""


class Text(unittest.TestCase):
    def test_names_from_gnu_ld(self):
        self.assertEqual(link_stubs.undefined_symbols(GNU_LD), ["D3DDevice_Clear", "XGetDevices"])

    def test_names_in_typographic_quotes(self):
        self.assertEqual(link_stubs.undefined_symbols(TYPOGRAPHIC), ["Beta"])

    def test_names_from_lld(self):
        self.assertEqual(link_stubs.undefined_symbols(LLD), ["gamma"])

    def test_nothing_undefined(self):
        self.assertEqual(link_stubs.undefined_symbols("collect2: error: ld returned 1 exit status\n"), [])

    def test_stub_source_calls_the_stop_function_with_each_name(self):
        text = link_stubs.stub_source(["alpha", "beta"], "stop_it", {})
        self.assertIn("void stop_it(const char *name) __attribute__((noreturn));", text)
        self.assertIn('void alpha(void) { stop_it("alpha"); }', text)
        self.assertIn('void beta(void) { stop_it("beta"); }', text)

    def test_stub_source_defines_a_data_name_as_zero_bytes(self):
        text = link_stubs.stub_source(["alpha", "table"], "stop_it", {"table": 64})
        self.assertIn("char table[64] __attribute__((aligned(16)));", text)
        self.assertNotIn("void table", text)
        self.assertIn('void alpha(void)', text)

    def test_data_option(self):
        self.assertEqual(link_stubs.parse_data(["a:8", "b"]), {"a": 8, "b": 16})
        for bad in ("a:0", "a:x", "1a", "a b:3", ":4"):
            with self.assertRaises(ValueError, msg=bad):
                link_stubs.parse_data([bad])


@unittest.skipUnless(CC, "no host C compiler")
class Links(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def compile(self, name, source):
        path = self.dir / f"{name}.c"
        path.write_text(source)
        obj = self.dir / f"{name}.o"
        subprocess.run([CC, "-c", str(path), "-o", str(obj)], check=True)
        return str(obj)

    def run_tool(self, objects, *extra, libs="", ldflags=""):
        arguments = ["--linker", CC, "--compiler", CC, "--ldflags", ldflags, "--libs", libs,
                     "--stubs", str(self.dir / "stubs.c"), "--report", str(self.dir / "report.txt"),
                     "--output", str(self.dir / "program"), "--name", "link_stubs.py", *extra, *objects]
        return link_stubs.main(arguments)

    def run_program(self):
        return subprocess.run([str(self.dir / "program")], capture_output=True, text=True)

    def report(self):
        return (self.dir / "report.txt").read_text().split()

    def test_a_complete_program_is_linked_as_it_is(self):
        main = self.compile("main", "int main(void) { return 0; }")
        self.assertEqual(self.run_tool([main]), 0)
        self.assertEqual(self.report(), [])
        self.assertFalse((self.dir / "stubs.c").exists())
        self.assertEqual(self.run_program().returncode, 0)

    def test_missing_functions_are_stubbed_and_stop_naming_themselves(self):
        main = self.compile("main", "void gamma_x(void); void alpha(void);\n"
                                    "int main(int c, char **v) { if (c > 1) alpha(); else gamma_x(); return 0; }")
        stop = self.compile("stop", STOP)
        self.assertEqual(self.run_tool([main, stop], "--stop-function", "stop_with"), 0)
        self.assertEqual(self.report(), ["alpha", "gamma_x"])
        ran = self.run_program()
        self.assertEqual((ran.returncode, ran.stdout), (3, "stopped by gamma_x\n"))
        ran = subprocess.run([str(self.dir / "program"), "x"], capture_output=True, text=True)
        self.assertEqual((ran.returncode, ran.stdout), (3, "stopped by alpha\n"))

    def test_the_default_stop_function_is_wiilibs(self):
        main = self.compile("main", "void alpha(void); int main(void) { alpha(); return 0; }")
        stop = self.compile("stop", STOP)
        self.assertEqual(self.run_tool([main, stop]), 0)
        ran = self.run_program()
        self.assertEqual((ran.returncode, ran.stdout), (4, "missing alpha\n"))

    def test_a_data_symbol_given_with_data_reads_as_zeros(self):
        main = self.compile("main", "extern int table[4];\n"
                                    "int main(void) { return table[0] | table[3]; }")
        stop = self.compile("stop", STOP)
        self.assertEqual(self.run_tool([main, stop], "--data", "table:16"), 0)
        self.assertEqual(self.report(), ["table"])
        self.assertEqual(self.run_program().returncode, 0)

    def test_a_response_file_lists_the_objects(self):
        main = self.compile("main", "void alpha(void); int main(void) { alpha(); return 0; }")
        stop = self.compile("stop", STOP)
        rsp = self.dir / "objects.rsp"
        rsp.write_text(" ".join(shlex.quote(obj) for obj in (main, stop)))
        self.assertEqual(self.run_tool([f"@{rsp}"]), 0)
        self.assertEqual(self.report(), ["alpha"])

    def test_a_failure_that_is_not_an_undefined_symbol_is_reported(self):
        first = self.compile("first", "int main(void) { return 0; }\nint twice(void) { return 1; }")
        second = self.compile("second", "int twice(void) { return 2; }")
        self.assertNotEqual(self.run_tool([first, second]), 0)
        self.assertFalse((self.dir / "stubs.c").exists())

    def test_a_second_link_that_fails_is_reported(self):
        # (the stop function itself is missing, so the stubs cannot link)
        main = self.compile("main", "void alpha(void); int main(void) { alpha(); return 0; }")
        self.assertNotEqual(self.run_tool([main]), 0)

    def test_a_stop_function_that_is_not_an_identifier_is_refused(self):
        main = self.compile("main", "int main(void) { return 0; }")
        with self.assertRaises(SystemExit):
            self.run_tool([main], "--stop-function", "a-b")

    @unittest.skipUnless(LIBRARIES, "no wl_platform libraries given")
    def test_against_the_stop_screen_of_wl_platform(self):
        platform, base, source = LIBRARIES
        main = self.dir / "main.c"
        main.write_text('#include "wl/platform/platform.h"\n'
                        "void D3DDevice_Clear(void);\n"
                        "int main(void) { wl_platform_log_init(0); D3DDevice_Clear(); return 0; }\n")
        obj = self.dir / "main.o"
        subprocess.run([CC, "-std=c11", f"-I{source}", "-c", str(main), "-o", str(obj)], check=True)
        libs = " ".join(shlex.quote(path) for path in (platform, base)) + " -lm -pthread"
        self.assertEqual(self.run_tool([str(obj)], libs=libs), 0)
        self.assertEqual(self.report(), ["D3DDevice_Clear"])
        ran = self.run_program()
        self.assertEqual(ran.returncode, 1)
        self.assertIn("D3DDevice_Clear is not provided yet", ran.stderr)


if __name__ == "__main__":
    unittest.main()
