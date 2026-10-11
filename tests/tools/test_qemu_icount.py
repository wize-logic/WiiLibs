#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/qemu_icount.py: the log parser (both of qemu's in_asm forms), the totals, the
functions and the differences; and, where qemu-ppc and a PowerPC Linux compiler are installed, a
loop counted under qemu, whose count grows by the same amount for each thousand passes."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import qemu_icount  # noqa: E402

# a block of 3 instructions at 0x1000 run twice, one of 2 at 0x2000 run once (qemu without a
# disassembler: the bytes as OBJD-T hex)
RAW = b"""\
IN: main
0x00001000:
OBJD-T: 7c0802a69421fff093e1000c

Trace 0: 0x7f0000000100 [00000000/0000000000001000/00006000/00000200] main
IN: helper
0x00002000:
OBJD-T: 386000014e800020

Trace 0: 0x7f0000000200 [00000000/0000000000002000/00006000/00000200] helper
Trace 0: 0x7f0000000100 [00000000/0000000000001000/00006000/00000200] main
"""

# the same with a disassembler: an instruction a line
DISASSEMBLED = b"""\
IN: main
0x00001000:  7c0802a6  mflr     r0
0x00001004:  9421fff0  stwu     r1, -0x10(r1)
0x00001008:  93e1000c  stw      r31, 0xc(r1)

Trace 0: 0x7f0000000100 [00000000/0000000000001000/00006000/00000200] main
IN: helper
0x00002000:  38600001  li       r3, 1
0x00002004:  4e800020  blr

Trace 0: 0x7f0000000200 [00000000/0000000000002000/00006000/00000200] helper
Trace 0: 0x7f0000000100 [00000000/0000000000001000/00006000/00000200] main
"""


class ParseTests(unittest.TestCase):
    def test_raw_and_disassembled_logs_count_the_same(self):
        for log in (RAW, DISASSEMBLED):
            counts = qemu_icount.parse(log.splitlines(keepends=True))
            self.assertEqual(counts.sizes, {0x1000: 3, 0x2000: 2})
            self.assertEqual(counts.runs, {0x1000: 2, 0x2000: 1})
            self.assertEqual(counts.total(), 8)
            self.assertEqual(counts.blocks()[0], (6, 0x1000, 3, 2))

    def test_a_block_translated_again_takes_its_new_size(self):
        log = RAW + b"IN: main\n0x00001000:  \nOBJD-T: 7c0802a6\n\n" + \
            b"Trace 0: 0x7f0000000300 [00000000/0000000000001000/00006000/00000200] main\n"
        counts = qemu_icount.parse(log.splitlines(keepends=True))
        self.assertEqual(counts.sizes[0x1000], 1)
        self.assertEqual(counts.total(), 3 * 1 + 2)

    def test_functions_and_differences(self):
        counts = qemu_icount.parse(RAW.splitlines(keepends=True))
        symbols = qemu_icount.Symbols([(0x2000, "helper"), (0x1000, "main")])
        functions = qemu_icount.per_function(counts, symbols)
        self.assertEqual(functions, {"main": 6, "helper": 2})
        self.assertEqual(symbols.offset(0x2004), "helper+0x4")
        self.assertEqual(symbols.name(0x10), "?")
        self.assertEqual(qemu_icount.difference(functions, {"main": 4, "gone": 1}),
                         {"main": 2, "helper": 2, "gone": -1})


LOOP = r"""
#include <stdlib.h>
int main(int argc, char **argv)
{
	volatile unsigned long sum = 0;
	unsigned long n, count = strtoul(argv[1], NULL, 0);

	for (n = 0; n < count; n++)
		sum += n * 3;
	/* (nothing printed: the count does not depend on how many digits it has) */
	return sum == 1;
}
"""


@unittest.skipUnless(shutil.which("qemu-ppc") and shutil.which("powerpc-linux-gnu-gcc"),
                     "needs qemu-ppc and powerpc-linux-gnu-gcc")
class QemuTests(unittest.TestCase):
    def test_a_loop_costs_the_same_for_each_thousand_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            source, program = Path(directory) / "loop.c", Path(directory) / "loop"
            source.write_text(LOOP)
            subprocess.run(["powerpc-linux-gnu-gcc", "-O2", "-mcpu=750", "-static", str(source), "-o", str(program)],
                           check=True)
            totals = [qemu_icount.run([str(program), str(count)], "qemu-ppc")[0].total() for count in (1000, 2000, 3000)]
            self.assertGreater(totals[1] - totals[0], 1000)
            self.assertEqual(totals[2] - totals[1], totals[1] - totals[0])
            output = subprocess.run([sys.executable, str(Path(qemu_icount.__file__)), "--base", f"{program} 1000",
                                     "--per", "1000", "--functions", "3", str(program), "2000"],
                                    capture_output=True, text=True, check=True).stdout
            self.assertIn(f"instructions {(totals[1] - totals[0]) / 1000:.1f}", output)
            self.assertIn("main", output)


if __name__ == "__main__":
    unittest.main()
