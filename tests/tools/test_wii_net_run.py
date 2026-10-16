#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/wii_net_run.py with a pretend wiiload and a pretend Wii (a small program that
speaks wl/platform/net_stream.h's protocol): both ways of connecting, a run that ends, one that
crashes, one that hangs, the protocol parser fed in pieces, and a file of runs."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import wii_net_run  # noqa: E402

FAKE_WII = textwrap.dedent('''
    import socket, sys, time
    args = dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a)
    net, play = args["net"], args.get("play", "ok")
    if net.startswith("listen:"):
        srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", int(net[7:]))); srv.listen(1); srv.settimeout(20)
        sock, _ = srv.accept()
    else:
        host, port = net.rsplit(":", 1)
        for _ in range(100):
            try:
                sock = socket.create_connection((host, int(port))); break
            except OSError:
                time.sleep(0.1)
    send = sock.sendall
    send(("HELLO WLNET1 %s\\n" % args.get("run", "run")).encode())
    send(b"[I][xbboot] title started\\n")
    send(b"[I][xbplay] play setting: " + play.encode() + b"\\n")
    if play == "ok":
        body = b"profile\\nEND\\nFILE x 1\\n"        # a file whose bytes look like the protocol
        send(b"FILE profile1.txt %d\\n" % len(body) + body)
        send(b"FILE play/shot.png 3\\nabc")
        send(b"[I][xbmain] the title left with 0\\n")
        send(b"END\\n")
        sys.exit(0)
    if play == "crash":
        send(b"[E][xbcrash] *** CRASH at 0x1234\\n")
    sock.settimeout(30)
    got = b""
    while b"QUIT\\n" not in got:
        data = sock.recv(100)
        if not data:
            sys.exit(0)
        got += data
    send(b"[I][wlnet] QUIT from the peer\\nEND\\n")
''')

FAKE_WIILOAD = textwrap.dedent('''
    #!{python}
    import os, subprocess, sys
    assert os.environ["WIILOAD"].startswith("tcp:"), os.environ.get("WIILOAD")
    assert os.path.exists(sys.argv[1]), sys.argv[1]
    subprocess.Popen([{python!r}, {wii!r}] + sys.argv[2:], start_new_session=True, stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
''')


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class StreamParserTests(unittest.TestCase):
    def parse(self, data, step):
        events = []
        files = {}

        def on_file(done, name=None):
            if done is None:
                files[name] = bytearray()

                class Sink:
                    def write(self, b):
                        files[name] += b

                    def close(self):
                        pass
                return Sink()
            events.append(("file", done))

        stream = wii_net_run.Stream(None)
        for i in range(0, len(data), step):
            stream.feed(data[i:i + step], lambda l: events.append(("line", l)), on_file,
                        lambda: events.append(("end",)), lambda h: events.append(("hello", h)))
        return events, files

    def test_pieces_of_any_size(self):
        data = b"HELLO WLNET1 a\n[I][t] one\nFILE a.txt 10\nEND\nFILE \n[I][t] two\r\nFILE z 0\nEND\n"
        want = None
        for step in (1, 2, 3, 7, 1000):
            events, files = self.parse(data, step)
            self.assertEqual(files["a.txt"], bytearray(b"END\nFILE \n"))
            self.assertEqual([e[1] for e in events if e[0] == "line"], ["[I][t] one", "[I][t] two"])
            self.assertEqual(events[0][0], "hello")
            self.assertEqual(events[-1], ("end",))
            self.assertIn(("file", "z"), events)
            if want is None:
                want = events
            self.assertEqual(events, want)

    def test_names_cannot_leave_the_folder(self):
        self.assertEqual(wii_net_run.safe_relative("../../etc/passwd"), os.path.join("etc", "passwd"))
        self.assertEqual(wii_net_run.safe_relative("play/shot.png"), os.path.join("play", "shot.png"))
        self.assertEqual(wii_net_run.safe_relative("/abs\\\\x"), os.path.join("abs", "x"))
        self.assertEqual(wii_net_run.safe_relative(".."), "unnamed")


class RunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        (root / "fake_wii.py").write_text(FAKE_WII)
        load = root / "wiiload"
        load.write_text(FAKE_WIILOAD.format(python=sys.executable, wii=str(root / "fake_wii.py")).lstrip())
        load.chmod(0o755)
        self.wiiload = str(load)
        self.dol = root / "t.dol"
        self.dol.write_bytes(b"dol")
        self.out = str(root / "out")
        # a pretend Homebrew Channel: accepts and closes
        self.hbc = socket.socket()
        self.hbc.bind(("127.0.0.1", 0))
        self.hbc.listen(5)
        self.hbc_port = self.hbc.getsockname()[1]
        threading.Thread(target=self.serve_hbc, daemon=True).start()

    def tearDown(self):
        self.hbc.close()
        self.tmp.cleanup()

    def serve_hbc(self):
        while True:
            try:
                c, _ = self.hbc.accept()
                c.close()
            except OSError:
                return

    def run_tool(self, *extra, mode="dial", expect=0):
        argv = ["--wii", "127.0.0.1", "--out", self.out, "--wiiload", self.wiiload, "--mode", mode,
                "--port", str(free_port()), "--hbc-port", str(self.hbc_port), "--connect-timeout", "20",
                "--quit-wait", "10"] + list(extra)
        self.assertEqual(wii_net_run.main(argv), expect)

    def result(self, name):
        return json.loads((Path(self.out) / name / "result.json").read_text())

    def check_ok_run(self, name):
        res = self.result(name)
        self.assertEqual(res["status"], "ok", res)
        log = (Path(self.out) / name / "log.txt").read_text()
        self.assertIn("[I][xbplay] play setting: ok", log)
        self.assertIn("the title left with 0", log)
        self.assertEqual((Path(self.out) / name / "files" / "profile1.txt").read_bytes(), b"profile\nEND\nFILE x 1\n")
        self.assertEqual((Path(self.out) / name / "files" / "play" / "shot.png").read_bytes(), b"abc")
        self.assertEqual(sorted(res["files"]), ["play/shot.png", "profile1.txt"])

    def test_dial_mode_a_run_that_ends(self):
        self.run_tool("--dol", str(self.dol), "--name", "ok1", "--arg", "play=ok")
        self.check_ok_run("ok1")
        self.assertIn("net=listen:", " ".join(self.result("ok1")["args"]))
        self.assertIn("run=ok1", self.result("ok1")["args"])

    def test_listen_mode_a_run_that_ends(self):
        self.run_tool("--dol", str(self.dol), "--name", "ok2", "--arg", "play=ok", "--pc-ip", "127.0.0.1",
                      mode="listen")
        self.check_ok_run("ok2")
        self.assertTrue(any(a.startswith("net=127.0.0.1:") for a in self.result("ok2")["args"]))

    def test_a_crash_is_sent_quit_and_fails(self):
        self.run_tool("--dol", str(self.dol), "--name", "bad", "--arg", "play=crash", "--fail-grace", "0.2",
                      expect=1)
        res = self.result("bad")
        self.assertEqual(res["status"], "failed", res)
        self.assertIn("CRASH", res["reason"])
        self.assertIn("QUIT from the peer", (Path(self.out) / "bad" / "log.txt").read_text())

    def test_silence_is_sent_quit(self):
        self.run_tool("--dol", str(self.dol), "--name", "hang", "--arg", "play=hang", "--idle", "1",
                      "--fail", "", expect=1)
        res = self.result("hang")
        self.assertEqual(res["status"], "idle", res)
        self.assertIn("idle for 1 s", res["reason"])

    def test_a_time_limit(self):
        self.run_tool("--dol", str(self.dol), "--name", "slow", "--arg", "play=hang", "--seconds", "1",
                      "--fail", "", expect=1)
        self.assertEqual(self.result("slow")["status"], "timeout")

    def test_a_file_of_runs_one_after_another(self):
        runs = Path(self.tmp.name) / "runs.txt"
        runs.write_text("# two runs\nfirst t.dol play=ok\nsecond t.dol 'play=ok' switch=noxf,profile\n")
        self.run_tool("--runs", str(runs))
        self.check_ok_run("first")
        self.check_ok_run("second")
        self.assertIn("switch=noxf,profile", self.result("second")["args"])

    def test_no_program_answers(self):
        silent = Path(self.tmp.name) / "silent"
        silent.write_text("#!/bin/sh\nexit 0\n")
        silent.chmod(0o755)
        argv = ["--wii", "127.0.0.1", "--out", self.out, "--wiiload", str(silent), "--dol", str(self.dol),
                "--name", "none", "--port", str(free_port()), "--connect-timeout", "1"]
        self.assertEqual(wii_net_run.main(argv), 1)
        self.assertIn("no connection", self.result("none")["reason"])

    def test_a_failing_wiiload(self):
        broken = Path(self.tmp.name) / "broken"
        broken.write_text("#!/bin/sh\necho no route >&2\nexit 3\n")
        broken.chmod(0o755)
        argv = ["--wii", "127.0.0.1", "--out", self.out, "--wiiload", str(broken), "--dol", str(self.dol),
                "--name", "nol", "--port", str(free_port())]
        self.assertEqual(wii_net_run.main(argv), 1)
        self.assertIn("wiiload failed: no route", self.result("nol")["reason"])


if __name__ == "__main__":
    unittest.main()
