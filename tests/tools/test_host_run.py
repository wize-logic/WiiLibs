#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/host_run.py: a run that ends itself keeps its status, and a run ends after a --fail
line, when its progress number stops growing, and at its limit, each with its own status and reason."""

import io
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import host_run  # noqa: E402


def program(body: str) -> list:
    return [sys.executable, "-u", "-c", "import time, sys\n" + body]


class HostRunTests(unittest.TestCase):
    def run_it(self, body, **limits):
        out = io.StringIO()
        options = {"fail": host_run.FAIL, "grace": 0.0, "progress": host_run.PROGRESS, "stall": 30.0,
                   "limit": 30.0}
        options.update(limits)
        status = host_run.run(program(body), out, **options)
        return status, out.getvalue()

    def test_a_run_that_ends_itself(self):
        status, text = self.run_it("print('tick 1'); print('tick 2'); sys.exit(7)")
        self.assertEqual(status, 7)
        self.assertIn("tick 2", text)
        self.assertIn("host_run: exit 7", text)

    def test_a_fail_line(self):
        status, text = self.run_it("print('[W][xbthread] stall: no scheduling for 20 s')\ntime.sleep(30)")
        self.assertEqual(status, host_run.EXIT_FAIL)
        self.assertIn("failed: [W][xbthread] stall: no scheduling", text)

    def test_no_progress(self):
        # (the number grows, then repeats: a repeated value is no progress)
        status, text = self.run_it("print('tick 5'); print('tick 9')\nwhile True:\n print('tick 9'); "
                                   "time.sleep(0.2)", stall=1.5)
        self.assertEqual(status, host_run.EXIT_STALL)
        self.assertIn("no progress", text)
        self.assertIn("(last 9)", text)

    def test_the_limit(self):
        status, text = self.run_it("n = 0\nwhile True:\n n += 1; print(f'tick {n}'); time.sleep(0.1)",
                                   limit=1.5)
        self.assertEqual(status, host_run.EXIT_LIMIT)
        self.assertIn("limit of 1 s reached", text)


if __name__ == "__main__":
    unittest.main()
