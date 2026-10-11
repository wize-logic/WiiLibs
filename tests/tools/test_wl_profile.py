#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/wl_profile.py: parsing a dump out of a log, the symbol table, stacks, the
aggregation, and the command line over a small ELF object built here."""

import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import wl_profile  # noqa: E402

TB = 60_750_000

# a program: main 1000..1100, update 2000..2200, physics 3000..3100 (a leaf), draw 4000..4100
SYMBOLS = [(0x1000, 0x100, "main"), (0x2000, 0x200, "update"), (0x3000, 0x100, "physics"),
           (0x4000, 0x100, "draw"), (0x5000, 0, "asm_label"), (0x5100, 0, "next_label")]


def symbols():
    return wl_profile.Symbols(SYMBOLS)


def line(tick, thread, pc, lr, *callers):
    return "WLPS " + " ".join(f"{word:x}" for word in (tick, thread, pc, lr, *callers))


class ParseTests(unittest.TestCase):
    def test_lines_are_found_among_other_text_and_prefixes(self):
        text = [
            "[I][halo] booting",
            "WLPROF 1 hz=1000 tbhz=60750000 depth=4 samples=2 total=2 dropped=0 thread=8123 note=a10 playing",
            "OSREPORT: " + line(100, 0x80aa, 0x2010, 0x1050, 0x1040),
            "some other log line",
            line(100 + TB // 1000, 0x80aa, 0x3010, 0x2050),
        ]
        profiles = wl_profile.parse_dump(text)
        self.assertEqual(len(profiles), 1)
        p = profiles[0]
        self.assertEqual((p.hz, p.timebase, p.note), (1000, TB, "a10 playing"))
        self.assertEqual(p.header["thread"], "8123")
        self.assertEqual(len(p.samples), 2)
        self.assertEqual((p.samples[0].tick, p.samples[1].tick), (0, TB // 1000))
        self.assertEqual((p.samples[0].pc, p.samples[0].lr, p.samples[0].callers), (0x2010, 0x1050, [0x1040]))
        self.assertEqual(p.samples[1].callers, [])

    def test_ticks_unwrap_the_32_bit_time_base(self):
        text = ["WLPROF 1 hz=1", line(0xfffffff0, 1, 1, 0), line(0x10, 1, 1, 0), line(0x20, 1, 1, 0)]
        samples = wl_profile.parse_dump(text)[0].samples
        self.assertEqual([s.tick for s in samples], [0, 0x20, 0x30])

    def test_two_headers_make_two_profiles_and_a_headerless_run_still_parses(self):
        text = ["WLPROF 1 hz=500", line(5, 1, 2, 3), "WLPROF 1 hz=250", line(9, 1, 2, 3), line(10, 1, 2, 3)]
        profiles = wl_profile.parse_dump(text)
        self.assertEqual([len(p.samples) for p in profiles], [1, 2])
        self.assertEqual(profiles[1].samples[1].tick, 1)
        headerless = wl_profile.parse_dump([line(5, 1, 2, 3)])
        self.assertEqual(len(headerless), 1)
        self.assertEqual(headerless[0].timebase, TB)  # (the Wii's by default)
        self.assertEqual(wl_profile.parse_dump(["nothing", "WLPS 1 2"]), [])  # (too short to be a sample)


class SymbolTests(unittest.TestCase):
    def test_lookup_by_range(self):
        s = symbols()
        self.assertEqual(s.lookup(0x1000), "main")
        self.assertEqual(s.lookup(0x10ff), "main")
        self.assertEqual(s.lookup(0x1100), wl_profile.UNKNOWN)  # past main's size, before update
        self.assertEqual(s.lookup(0x0fff), wl_profile.UNKNOWN)
        self.assertEqual(s.lookup(0x2100), "update")
        self.assertEqual(s.lookup(0x5050), "asm_label")  # (no size: to the next symbol)
        self.assertEqual(s.lookup(0x5100), "next_label")

    def test_the_sized_symbol_wins_at_one_address(self):
        s = wl_profile.Symbols([(0x10, 0, "alias"), (0x10, 0x20, "real")])
        self.assertEqual(s.lookup(0x18), "real")


class StackTests(unittest.TestCase):
    def sample(self, pc, lr, *callers):
        return wl_profile.Sample(0, 1, pc, lr, list(callers))

    def test_a_leaf_function_gets_its_link_register_as_the_caller(self):
        # physics (a leaf, no frame) was called from update; update's own caller is in the chain
        stack = wl_profile.stack_of(self.sample(0x3010, 0x2050, 0x1040), symbols())
        self.assertEqual([name for _, name in stack], ["physics", "update", "main"])

    def test_a_link_register_inside_the_function_is_dropped(self):
        # update has called physics before; its lr points back into update, the chain gives main
        stack = wl_profile.stack_of(self.sample(0x2010, 0x2050, 0x1040), symbols())
        self.assertEqual([name for _, name in stack], ["update", "main"])

    def test_a_link_register_repeating_the_chain_is_not_doubled(self):
        stack = wl_profile.stack_of(self.sample(0x2004, 0x1040, 0x1040), symbols())
        self.assertEqual([hex(a) for a, _ in stack], ["0x2004", "0x1040"])

    def test_callers_are_looked_up_at_the_call_not_after_it(self):
        # a call that ends main: the return address 0x1100 is already past main
        stack = wl_profile.stack_of(self.sample(0x3010, 0, 0x1100), symbols())
        self.assertEqual([name for _, name in stack], ["physics", "main"])


def synthetic_profile():
    # 10 samples: 6 in physics (called from update, from main), 2 in update itself, 1 in draw, 1 unknown
    samples = []
    tick = 0
    for _ in range(6):
        samples.append(wl_profile.Sample(tick, 1, 0x3010, 0x2050, [0x1040]))
        tick += TB // 100
    for _ in range(2):
        samples.append(wl_profile.Sample(tick, 1, 0x2010, 0x2050, [0x1040]))
        tick += TB // 100
    samples.append(wl_profile.Sample(tick, 2, 0x4010, 0x1040, []))
    tick += TB // 100
    samples.append(wl_profile.Sample(tick, 1, 0x9000, 0, []))
    return wl_profile.Profile(header={"hz": "100", "tbhz": str(TB), "depth": "4"}, samples=samples)


class AggregateTests(unittest.TestCase):
    def test_self_inclusive_callers_and_callees(self):
        r = wl_profile.aggregate(synthetic_profile(), symbols())
        self.assertEqual(r.total, 10)
        self.assertEqual(dict(r.self_counts), {"physics": 6, "update": 2, "draw": 1, wl_profile.UNKNOWN: 1})
        self.assertEqual(r.inclusive_counts["main"], 9)  # (the unknown pc is nowhere under main)
        self.assertEqual(r.inclusive_counts["update"], 8)
        self.assertEqual(r.inclusive_counts["physics"], 6)
        self.assertEqual(dict(r.callers_of["physics"]), {"update": 6})
        self.assertEqual(dict(r.callers_of["update"]), {"main": 8})
        self.assertEqual(dict(r.callees_of["update"]), {"physics": 6, "(self)": 2})
        self.assertEqual(dict(r.callers_of["draw"]), {"main": 1})
        self.assertAlmostEqual(r.seconds, 9 / 100)
        self.assertEqual(r.threads, [(1, 9), (2, 1)])

    def test_a_thread_filter(self):
        r = wl_profile.aggregate(synthetic_profile(), symbols(), thread=2)
        self.assertEqual((r.total, dict(r.self_counts)), (1, {"draw": 1}))

    def test_recursion_counts_a_function_once_a_sample(self):
        p = wl_profile.Profile(header={"tbhz": str(TB)},
                               samples=[wl_profile.Sample(0, 1, 0x2010, 0, [0x2020, 0x2030, 0x1040])])
        r = wl_profile.aggregate(p, symbols())
        self.assertEqual(r.inclusive_counts["update"], 1)
        self.assertEqual(r.folded["main;update;update;update"], 1)

    def test_tree_and_folded_stacks(self):
        r = wl_profile.aggregate(synthetic_profile(), symbols())
        self.assertEqual(r.folded["main;update;physics"], 6)
        self.assertEqual(r.folded["main;update;update"] if "main;update;update" in r.folded else 0, 0)
        main = r.tree["children"]["main"]
        self.assertEqual(main["count"], 9)
        self.assertEqual(main["children"]["update"]["children"]["physics"]["count"], 6)
        text = "\n".join(wl_profile.render_tree(r.tree, r.total, 15.0, 8))
        self.assertIn("physics", text)
        self.assertNotIn("draw", text)  # (10% of the samples: under the threshold)

    def test_timeline_buckets(self):
        r = wl_profile.aggregate(synthetic_profile(), symbols(), bucket_seconds=0.05)
        self.assertEqual([sum(b.values()) for b in r.timeline], [5, 5])
        self.assertEqual(r.timeline[0].most_common(1)[0][0], "physics")


C_SOURCE = """
__attribute__((noinline)) int leaf_work(int x) { return x * 3 + 1; }
__attribute__((noinline)) int caller_work(int x) { return leaf_work(x) + leaf_work(x + 1); }
"""


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class ElfTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        source = Path(self.dir.name) / "fixture.c"
        source.write_text(C_SOURCE)
        self.obj = Path(self.dir.name) / "fixture.elf"
        # (linked, so the functions have addresses of their own; no C library needed)
        subprocess.run(["cc", "-O1", "-nostdlib", "-static", "-Wl,-e,caller_work", "-o", str(self.obj), str(source)],
                       check=True)
        try:
            import elftools  # noqa: F401
        except ImportError:
            self.skipTest("no pyelftools")

    def test_the_elf_symbol_table_gives_functions_with_sizes(self):
        s = wl_profile.Symbols.from_elf(str(self.obj))
        self.assertIn("leaf_work", s.names)
        self.assertIn("caller_work", s.names)
        start = s.starts[s.names.index("leaf_work")]
        size = s.sizes[s.names.index("leaf_work")]
        self.assertGreater(size, 0)
        self.assertEqual(s.lookup(start), "leaf_work")
        self.assertEqual(s.lookup(start + size - 1), "leaf_work")

    def test_the_command_line_end_to_end(self):
        s = wl_profile.Symbols.from_elf(str(self.obj))
        leaf = s.starts[s.names.index("leaf_work")]
        caller = s.starts[s.names.index("caller_work")]
        lines = ["WLPROF 1 hz=1000 tbhz=60750000 depth=4 samples=4 total=4 dropped=0 thread=1 note=fixture"]
        for index in range(4):
            lines.append(line(1000 * index, 7, leaf + 2, caller + 4, 0x80000000))
        dump = Path(self.dir.name) / "dump.txt"
        dump.write_text("\n".join(lines) + "\n")
        folded = Path(self.dir.name) / "folded.txt"
        out = io.StringIO()
        saved = sys.stdout
        sys.stdout = out
        try:
            code = wl_profile.main([str(self.obj), str(dump), "--top", "5", "--callers", "leaf_work", "--tree",
                                    "--timeline", "--collapsed", str(folded)])
        finally:
            sys.stdout = saved
        text = out.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("samples 4", text)
        self.assertRegex(text, r"4\s+100\.0%\s+leaf_work")
        self.assertRegex(text, r"called from\n\s+4\s+100\.0%\s+caller_work")
        self.assertIn("call tree", text)
        self.assertIn("(fixture)", text)
        self.assertEqual(folded.read_text().split()[-1], "4")
        self.assertIn("leaf_work", folded.read_text())

    def test_empty_input_is_an_error(self):
        dump = Path(self.dir.name) / "empty.txt"
        dump.write_text("no profile here\n")
        err = io.StringIO()
        saved = sys.stderr
        sys.stderr = err
        try:
            code = wl_profile.main([str(self.obj), str(dump)])
        finally:
            sys.stderr = saved
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
