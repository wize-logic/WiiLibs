#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Runs a host program (a recompiled title's level run) and ends it when it can no longer get anywhere.

The host's version of dolphin_run.py's --fail and --idle. The program's output goes to --log (or to
stdout). The run ends:
- --fail-grace seconds after a line that matches --fail (by default the runtime's crash, stall and
  freeze reports and a stopped play script; the grace lets the freeze watch's report reach the log);
- when the number --progress captures (by default the play harness's `tick N`) has not grown for
  --stall seconds: a hang, or a script that no longer moves the game;
- after --limit seconds in all.

Ending is SIGINT to the program's process group, then SIGKILL 10 s later. The last line, in the log
and on stderr, says why the run ended. The exit status is the program's own when it ended itself, 3
after a --fail line, 4 for no progress and 5 at the limit.

usage: host_run.py [--log FILE] [--fail RE] [--progress RE] [--stall S] [--limit S] -- program args...
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time

FAIL = r"freeze: no new frame|stall: no scheduling|GUEST CRASH|script stopped at line|\*\*\* CRASH"
PROGRESS = r"\btick (\d+)"
EXIT_FAIL, EXIT_STALL, EXIT_LIMIT = 3, 4, 5


class Watch:
    """what the program's lines say so far: the first --fail line's time and the progress number's best"""

    def __init__(self, fail: str, progress: str, started: float):
        self.fail = re.compile(fail) if fail else None
        self.progress = re.compile(progress) if progress else None
        self.failed_at = None
        self.fail_line = ""
        self.best = None
        self.moved_at = started
        self.lock = threading.Lock()

    def line(self, text: str, now: float) -> None:
        with self.lock:
            if self.fail is not None and self.failed_at is None and self.fail.search(text):
                self.failed_at = now
                self.fail_line = text.strip()
            if self.progress is not None:
                for match in self.progress.finditer(text):
                    value = int(match.group(1))
                    if self.best is None or value > self.best:
                        self.best = value
                        self.moved_at = now

    def verdict(self, now: float, started: float, grace: float, stall: float, limit: float):
        """(exit status, reason) once the run should end, else None"""
        with self.lock:
            if self.failed_at is not None and now - self.failed_at >= grace:
                return EXIT_FAIL, f"failed: {self.fail_line}"
            if self.progress is not None and stall > 0 and now - self.moved_at >= stall:
                return EXIT_STALL, f"no progress for {int(now - self.moved_at)} s (last {self.best})"
            if limit > 0 and now - started >= limit:
                return EXIT_LIMIT, f"limit of {int(limit)} s reached (progress {self.best})"
        return None


def stop(process: subprocess.Popen) -> None:
    for sig, wait in ((signal.SIGINT, 10.0), (signal.SIGKILL, 5.0)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        try:
            process.wait(wait)
            return
        except subprocess.TimeoutExpired:
            continue


def run(command: list, out, fail: str, grace: float, progress: str, stall: float, limit: float) -> int:
    started = time.monotonic()
    watch = Watch(fail, progress, started)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True, text=True, errors="replace")

    def pump():
        for text in process.stdout:
            out.write(text)
            out.flush()
            watch.line(text, time.monotonic())

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    status, reason = None, ""
    while process.poll() is None:
        ended = watch.verdict(time.monotonic(), started, grace, stall, limit)
        if ended is not None:
            status, reason = ended
            stop(process)
            break
        time.sleep(0.5)
    reader.join(5.0)
    if status is None:
        status = process.wait()
        reason = f"exit {status}"
    note = f"host_run: {reason} after {int(time.monotonic() - started)} s\n"
    out.write(note)
    out.flush()
    if out is not sys.stdout:
        sys.stderr.write(note)
    return status


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--log", help="the program's output goes here (default: stdout)")
    parser.add_argument("--fail", default=FAIL, help="a line matching this ends the run, failed ('' for none)")
    parser.add_argument("--fail-grace", type=float, default=15.0,
                        help="seconds the run goes on after a --fail line (default 15)")
    parser.add_argument("--progress", default=PROGRESS,
                        help="a regular expression whose group 1 is a number that grows while the run gets "
                             "somewhere (default: the play harness's 'tick N'; '' for none)")
    parser.add_argument("--stall", type=float, default=300.0,
                        help="end the run when the progress number has not grown for this many seconds "
                             "(default 300; 0 never)")
    parser.add_argument("--limit", type=float, default=2700.0,
                        help="the longest a run lasts in seconds (default 2700, 45 min; 0 none)")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- the program and its arguments")
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no program given")
    if args.log:
        with open(args.log, "w", encoding="utf-8") as out:
            return run(command, out, args.fail, args.fail_grace, args.progress, args.stall, args.limit)
    return run(command, sys.stdout, args.fail, args.fail_grace, args.progress, args.stall, args.limit)


if __name__ == "__main__":
    sys.exit(main())
