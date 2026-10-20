#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the parts of tools/dolphin_run.py that need no Dolphin: which files of the card a run
changed (card_changes) and their copy out (copy_out, --sd-out), the copy in of the card (copy_tree), and
when a run ends early (RunWatch), the wall-clock stamps of marked lines (LineStamps, --stamp), and the command
line's refusal of --sd-out without --sd."""

import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import dolphin_run  # noqa: E402


def write(root: Path, relative: str, data: bytes) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class CardChangeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.card = self.root / "card"
        for tree in (self.source, self.card):
            write(tree, "apps/app/boot.dol", b"program")
            write(tree, "apps/app/config.toml", b"a = 1\n")
            write(tree, "apps/app/maps/big.map", b"x" * 100)

    def tearDown(self):
        self.temp.cleanup()

    def test_nothing_changed_is_nothing(self):
        self.assertEqual([], dolphin_run.card_changes(self.source, self.card))

    def test_a_new_file_in_a_new_folder_is_found(self):
        write(self.card, "apps/app/logs/log-00.log", b"line\n")
        self.assertEqual([Path("apps/app/logs/log-00.log")], dolphin_run.card_changes(self.source, self.card))

    def test_a_file_of_another_size_or_other_bytes_is_found(self):
        write(self.card, "apps/app/config.toml", b"a = 12\n")      # longer
        write(self.card, "apps/app/boot.dol", b"PROGRAM")           # the same size, other bytes
        self.assertEqual([Path("apps/app/boot.dol"), Path("apps/app/config.toml")],
                         dolphin_run.card_changes(self.source, self.card))

    def test_a_large_file_of_the_same_size_counts_as_the_same(self):
        big = b"y" * (dolphin_run.SMALL_FILE + 1)
        write(self.source, "apps/app/maps/large.map", big)
        write(self.card, "apps/app/maps/large.map", b"z" * len(big))
        self.assertEqual([], dolphin_run.card_changes(self.source, self.card))
        write(self.card, "apps/app/maps/large.map", b"z" * (len(big) + 1))
        self.assertEqual([Path("apps/app/maps/large.map")], dolphin_run.card_changes(self.source, self.card))

    def test_a_file_the_run_removed_is_not_a_change(self):
        (self.card / "apps/app/config.toml").unlink()
        self.assertEqual([], dolphin_run.card_changes(self.source, self.card))

    def test_copy_out_copies_only_the_changes_and_overwrites_nothing_else(self):
        write(self.card, "apps/app/logs/log-00.log", b"line\n")
        write(self.card, "apps/app/config.toml", b"a = 12\n")
        out = self.root / "out"
        write(out, "apps/app/other.txt", b"mine")
        write(out, "apps/app/logs/log-00.log", b"old")
        copied = dolphin_run.copy_out(self.source, self.card, out)
        self.assertEqual([Path("apps/app/config.toml"), Path("apps/app/logs/log-00.log")], copied)
        self.assertEqual(b"line\n", (out / "apps/app/logs/log-00.log").read_bytes())
        self.assertEqual(b"a = 12\n", (out / "apps/app/config.toml").read_bytes())
        self.assertEqual(b"mine", (out / "apps/app/other.txt").read_bytes())
        self.assertFalse((out / "apps/app/boot.dol").exists())
        self.assertFalse((out / "apps/app/maps/big.map").exists())


class CopyTreeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.copy = self.root / "copy"
        write(self.source, "hdd/Z/cache000.map", b"c" * 64)
        write(self.source, "play.cfg", b"script:a\n")
        old = 1_000_000_000
        for path in self.source.rglob("*"):
            os.utime(path, (old, old))

    def tearDown(self):
        self.temp.cleanup()

    def test_an_unchanged_copy_is_kept(self):
        dolphin_run.copy_tree(self.source, self.copy)
        cache = self.copy / "hdd/Z/cache000.map"
        stamp = cache.stat().st_mtime_ns
        dolphin_run.copy_tree(self.source, self.copy)
        self.assertEqual(stamp, cache.stat().st_mtime_ns)

    def test_a_file_a_run_wrote_back_is_copied_again(self):
        # Dolphin syncs the card back into the copy when it stops: a cache the game rewrote (or left half
        # written) has the source's size and a later time, and must not reach the next run
        dolphin_run.copy_tree(self.source, self.copy)
        cache = self.copy / "hdd/Z/cache000.map"
        cache.write_bytes(b"x" * 64)
        os.utime(cache, (2_000_000_000, 2_000_000_000))
        dolphin_run.copy_tree(self.source, self.copy)
        self.assertEqual(b"c" * 64, cache.read_bytes())

    def test_what_the_source_lacks_is_removed(self):
        dolphin_run.copy_tree(self.source, self.copy)
        write(self.copy, "hdd/raw5.bin", b"partition")
        dolphin_run.copy_tree(self.source, self.copy)
        self.assertFalse((self.copy / "hdd/raw5.bin").exists())


class RunWatchTests(unittest.TestCase):
    FREEZE = re.compile(r"freeze: no new frame|\*\*\* CRASH")

    def test_a_fail_line_ends_the_run_after_the_grace(self):
        watch = dolphin_run.RunWatch(self.FREEZE, 15, 0, 0.0, 1800.0)
        self.assertEqual("", watch.update(["[I][play] ticks 30"], 10.0))
        self.assertEqual(1800.0, watch.deadline)
        self.assertIn("--fail line", watch.update(["[I][play] ticks 30", "[W][xbirq] freeze: no new frame for 30 s"], 40.0))
        self.assertTrue(watch.failed)
        self.assertEqual(55.0, watch.deadline)
        watch.update(["a", "b", "c"], 50.0)  # a later line keeps the earlier deadline
        self.assertEqual(55.0, watch.deadline)

    def test_a_silent_program_ends_the_run_when_idle_is_set(self):
        watch = dolphin_run.RunWatch(None, 15, 60, 0.0, 1800.0)
        watch.update(["one"], 5.0)
        watch.update(["one"], 64.0)
        self.assertFalse(watch.idle_ended)
        self.assertIn("no new OSREPORT line", watch.update(["one"], 66.0))
        self.assertTrue(watch.idle_ended and watch.failed)

    def test_lines_that_keep_coming_never_end_it(self):
        watch = dolphin_run.RunWatch(self.FREEZE, 15, 60, 0.0, 1800.0)
        for second in range(0, 600, 10):
            watch.update(["status"] * (second + 1), float(second))
        self.assertFalse(watch.failed)
        self.assertEqual(1800.0, watch.deadline)


class LineStampTests(unittest.TestCase):
    BENCH = re.compile(r"run 1 frame 0:|summary: [0-9]")

    def test_each_matching_line_is_stamped_once_when_it_first_appears(self):
        stamps = dolphin_run.LineStamps(self.BENCH, 100.0)
        lines = ["[I][xboot] boot", "[I][xbbench] run 1 frame 0: 4.6 ms"]
        stamps.update(lines[:1], 110.0)
        stamps.update(lines, 130.25)
        stamps.update(lines, 131.0)  # (seen before: not stamped again)
        lines += ["[I][xbbench] run 2 frame 0: 2.3 ms", "[I][xbbench] summary: 20 runs of 2 frames",
                  "[I][xbbench] summary: mean 1 2 3"]
        stamps.update(lines, 165.75)
        self.assertEqual([30.25, 65.75], [seconds for seconds, _, _, _ in stamps.stamps])
        report = stamps.report()
        self.assertIn("stamp    30.25 s: [I][xbbench] run 1 frame 0: 4.6 ms", report[0])
        self.assertIn("(+35.50): [I][xbbench] summary: 20 runs", report[1])
        self.assertEqual("stamps: 2 lines, 35.50 s from the first to the last", report[2])

    def test_a_line_seen_only_at_the_end_is_marked(self):
        stamps = dolphin_run.LineStamps(self.BENCH, 0.0)
        stamps.update(["[I][xbbench] summary: 1 runs"], 9.0, late=True)
        self.assertIn("after the run's end", stamps.report()[0])
        self.assertEqual(["stamps: no line matched --stamp"], dolphin_run.LineStamps(self.BENCH, 0.0).report())

    def test_a_line_still_being_written_waits_for_its_end(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / "dolphin.log"
            log.write_text("00:00:000 Core/HLE: N[OSREPORT]: [I][xbbench] run 1 frame 0: 4.6 ms\n"
                           "00:01:000 Core/HLE: N[OSREPORT]: [I][xbbench] summ")
            self.assertEqual(1, len(dolphin_run.complete_lines(log)))
            self.assertEqual(2, len(dolphin_run.complete_lines(log, whole=True)))
            with log.open("a") as out:
                out.write("ary: 20 runs\n")
            lines = dolphin_run.complete_lines(log)
            self.assertEqual(2, len(lines))
            self.assertEqual([], dolphin_run.complete_lines(Path(temp) / "none.log"))
            # (Dolphin's own stamps: the bench's part by its log's clock, to the millisecond)
            stamps = dolphin_run.LineStamps(self.BENCH, 0.0)
            stamps.update(lines, 3.0)
            self.assertEqual("[I][xbbench] summary: 20 runs", stamps.stamps[1][2])
            self.assertIn("(Dolphin's log clock: 1.000 s)", stamps.report()[-1])

    def test_the_log_clock_turns_over_the_hour(self):
        stamps = dolphin_run.LineStamps(self.BENCH, 0.0)
        stamps.update(["59:58:500 Core/HLE: N[OSREPORT]: run 1 frame 0: 1 ms",
                       "00:30:250 Core/HLE: N[OSREPORT]: summary: 20 runs"], 40.0)
        self.assertIn("log clock +31.750", stamps.report()[1])


class GfxIniTests(unittest.TestCase):
    def test_the_defaults_and_an_override(self):
        text = dolphin_run.gfx_ini(False)
        self.assertIn("EFBToTextureEnable = False", text)
        text = dolphin_run.gfx_ini(False, hacks={"EFBToTextureEnable": "True", "ImmediateXFBEnable": "True"})
        self.assertIn("EFBToTextureEnable = True", text)
        self.assertNotIn("EFBToTextureEnable = False", text)
        self.assertIn("ImmediateXFBEnable = True", text)
        text = dolphin_run.gfx_ini(True, hacks={"Settings.CPUCull": "False"})
        settings = text[text.index("[Settings]"):]
        self.assertIn("CPUCull = False", settings)
        self.assertIn("DumpFramesAsImages = True", settings)
        self.assertNotIn("CPUCull", text[:text.index("[Settings]")])

    def test_the_frame_counter(self):
        self.assertIn("ShowFPS = True", dolphin_run.gfx_ini(False))
        self.assertNotIn("ShowFPS", dolphin_run.gfx_ini(False, fps=False))
        self.assertIn("ShowFPS = False", dolphin_run.gfx_ini(False, hacks={"Settings.ShowFPS": "False"}))


class XwdTests(unittest.TestCase):
    def test_a_32_bit_dump_becomes_an_rgb_png(self):
        # (a 2x1 ZPixmap, least significant byte first, r<<16 g<<8 b, a window name, no colour map, a padded line)
        name = b"Dolphin\0"
        header = [100 + len(name), 7, 2, 24, 2, 1, 0, 0, 32, 0, 32, 32, 12, 4, 0xff0000, 0xff00, 0xff, 8, 0, 0,
                  2, 1, 0, 0, 0]
        pixels = (0x102030).to_bytes(4, "little") + (0xa0b0c0).to_bytes(4, "little") + bytes(4)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "shot.png"
            dolphin_run.xwd_to_png(struct.pack(">25I", *header) + name + pixels, output)
            png = output.read_bytes()
        self.assertEqual(b"\x89PNG", png[:4])
        self.assertEqual((2, 1, 8, 2), struct.unpack(">IIBB", png[16:26]))
        start = png.index(b"IDAT") + 4
        self.assertEqual(b"\0\x10\x20\x30\xa0\xb0\xc0", zlib.decompress(png[start:start + struct.unpack(
            ">I", png[start - 8:start - 4])[0]]))


class CommandLineTests(unittest.TestCase):
    def test_sd_out_without_sd_is_refused_before_dolphin_is_looked_for(self):
        result = subprocess.run([sys.executable, str(TOOLS / "dolphin_run.py"), "program.dol", "--sd-out", "out"],
                                capture_output=True, text=True)
        self.assertEqual(2, result.returncode)
        self.assertIn("--sd-out needs --sd", result.stderr)


if __name__ == "__main__":
    unittest.main()
