#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Runs programs on a real Wii over Wi-Fi: no SD card swaps.

For each run it sends the DOL with wiiload (the Homebrew Channel's loader, WIILOAD=tcp:<wii ip>)
and the run's options as arguments, takes the program's log and the files it writes over one TCP
connection (wl/platform/net_stream.h), saves them under an output folder, ends the run on its end
line, a failure pattern or a silence, and sends QUIT so that the program leaves for the Homebrew
Channel and the next run can be loaded.

  tools/wii_net_run.py --wii 192.168.1.50 --dol halo.dol --name a30 \\
      --arg 'play=replay:play/a30_peak.cap 5' --arg switch=nooverlap,profile --out ~/wl-work/netruns

  tools/wii_net_run.py --wii 192.168.1.50 --runs runs.txt --out ~/wl-work/netruns

A runs file has one run a line: `name dol option option ...` (shell quoting; `#` starts a comment;
a relative dol is relative to the file). The options are those of wl/xboxrt/runopts.h: play=,
switch=, file=, run=. The tool adds net= and run= itself.

Which end connects (--mode):
  dial    (default) the program listens (net=listen:<port>) and this tool connects to the Wii. Works
          through any NAT on this side, WSL2's default included: only an outgoing connection.
  listen  this tool listens on --port and the program connects to --pc-ip (net=<pc ip>:<port>). The
          PC must be reachable from the Wii: Windows python.exe (with a firewall rule), WSL2 mirrored
          networking (.wslconfig: [wsl2] networkingMode=mirrored, then wsl --shutdown), or `netsh
          interface portproxy add v4tov4 listenport=7777 connectaddress=<WSL address>` plus a
          firewall rule. --pc-ip is the address the Wii sees (default: the local address of a route
          to the Wii).

WSL2 with its default NAT (checked here): a program on the LAN cannot reach a listener in WSL, and WSL
cannot reach a listener on Windows through Windows' firewall (a Windows program, Dolphin included, that
listens on its own port). What does work: WSL connecting out to the LAN (dial: the real Wii), and
Windows connecting to 127.0.0.1:<port> of a listener in WSL (listen: Dolphin, with no wiiload; start it
with netrun.txt holding `net=127.0.0.1:7777` and run this tool with --no-load --mode listen
--pc-ip 127.0.0.1 --port 7777). So the Wii uses dial and nothing is set up on the PC.

Output, per run, in <out>/<name>/: log.txt (every log line), files/<name> (what the run wrote:
profile<N>.txt, play/<screenshot>, ...), result.json (status, reason, seconds, counts). The exit
status is 0 when every run ended "ok".
"""
import argparse
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import time

DEFAULT_FAIL = r"freeze: no new frame|stall: no scheduling|GUEST CRASH|script stopped at line|\*\*\* CRASH"
HBC_PORT = 4299  # the Homebrew Channel's wiiload port


class Stream:
    """The protocol of net_stream.h on a connected socket: log lines, FILE records, END."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()
        self.need = 0  # bytes of a file still to come
        self.file = None
        self.file_name = None
        self.closed = False

    def feed(self, data, on_line, on_file, on_end, on_hello):
        self.buf += data
        while True:
            if self.file is not None:
                take = min(self.need, len(self.buf))
                if take:
                    self.file.write(self.buf[:take])
                    del self.buf[:take]
                    self.need -= take
                if self.need:
                    return
                self.file.close()
                on_file(self.file_name)
                self.file = None
                continue
            nl = self.buf.find(b"\n")
            if nl < 0:
                if len(self.buf) > 4096:  # not a line of ours: keep the tail
                    on_line(self.buf.decode("utf-8", "replace"))
                    self.buf.clear()
                return
            line = bytes(self.buf[:nl]).decode("utf-8", "replace").rstrip("\r")
            del self.buf[: nl + 1]
            if line.startswith("FILE "):
                parts = line.split(" ")
                if len(parts) == 3 and parts[2].isdigit():
                    self.file_name, self.need = parts[1], int(parts[2])
                    self.file = on_file(None, self.file_name)
                    continue
            if line.startswith("HELLO WLNET1"):
                on_hello(line)
            elif line == "END":
                on_end()
            else:
                on_line(line)


def safe_relative(name):
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else "unnamed"


def local_address_for(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def wait_hbc(host, port, seconds):
    """True once the Homebrew Channel's wiiload port answers (the program has left)."""
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection((host, port), timeout=2).close()
            return True
        except OSError:
            time.sleep(1)
    return False


def run_one(args, name, dol, options, log):
    out_dir = os.path.join(args.out, name)
    files_dir = os.path.join(out_dir, "files")
    os.makedirs(files_dir, exist_ok=True)
    fail_re = re.compile(args.fail) if args.fail else None
    end_re = re.compile(args.end) if args.end else None
    listener = None
    if args.mode == "listen":
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.listen_addr, args.port))
        listener.listen(1)
        pc_ip = args.pc_ip or local_address_for(args.wii)
        net = "net=%s:%d" % (pc_ip, args.port)
    else:
        net = "net=listen:%d" % args.port
    argv = [net, "run=" + name] + options
    env = dict(os.environ, WIILOAD="tcp:" + args.wii)
    log("%s: wiiload %s %s" % (name, dol, " ".join(shlex.quote(a) for a in argv)))
    t0 = time.time()
    result = {"name": name, "dol": dol, "args": argv, "status": "error", "reason": "", "seconds": 0.0,
              "lines": 0, "files": [], "connect_seconds": None}
    if not args.no_load:
        load = subprocess.run([args.wiiload, dol] + argv, env=env, capture_output=True, text=True)
        if load.returncode != 0:
            result["reason"] = "wiiload failed: " + (load.stderr or load.stdout).strip()[:200]
            return finish(args, out_dir, result, t0, listener, None)

    # the connection
    sock = None
    deadline = time.time() + args.connect_timeout
    while sock is None and time.time() < deadline:
        try:
            if listener is not None:
                listener.settimeout(1)
                sock, _ = listener.accept()
            else:
                sock = socket.create_connection((args.wii, args.port), timeout=2)
        except OSError:
            time.sleep(0.5 if listener is None else 0)
    if sock is None:
        result["reason"] = "no connection from the program in %d s" % args.connect_timeout
        return finish(args, out_dir, result, t0, listener, None)
    result["connect_seconds"] = round(time.time() - t0, 1)
    sock.settimeout(1.0)

    log_file = open(os.path.join(out_dir, "log.txt"), "w", encoding="utf-8", errors="replace")
    state = {"end": False, "fail_at": None, "reason": "", "quit_at": None, "last": time.time()}
    start = time.time()

    def on_line(line):
        result["lines"] += 1
        log_file.write(line + "\n")
        if args.echo:
            print("  " + line, flush=True)
        if fail_re is not None and state["fail_at"] is None and fail_re.search(line):
            state["fail_at"] = time.time()
            state["reason"] = "failure pattern: " + line[:160]
        if end_re is not None and not state["end"] and end_re.search(line):
            state["end"] = True
            state["reason"] = "end pattern: " + line[:160]

    def on_file(done, name_=None):
        if done is not None:  # a file finished
            result["files"].append(done)
            return None
        path = os.path.join(files_dir, safe_relative(name_))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return open(path, "wb")

    def on_end():
        state["end"] = True
        state["reason"] = state["reason"] or "END"
        state["got_end"] = True

    def verdict(ended):
        """What the run was: a failure line, a silence or the time limit outrank a clean END."""
        if state["fail_at"] is not None:
            return "failed"
        if state.get("idle"):
            return "idle"
        if state.get("timeout"):
            return "timeout"
        return "ok" if ended else "lost"

    stream = Stream(sock)
    status = None
    while True:
        now = time.time()
        if state.get("got_end"):
            status = verdict(True)
            break
        if state["fail_at"] is not None and state["quit_at"] is None and now - state["fail_at"] >= args.fail_grace:
            sock_send(sock, b"QUIT\n")
            state["quit_at"] = now
        if state["end"] and state["quit_at"] is None and not state.get("got_end") and now - state["last"] > args.end_grace:
            sock_send(sock, b"QUIT\n")
            state["quit_at"] = now
        if state["quit_at"] is None and now - state["last"] > args.idle:
            state["reason"] = "idle for %d s" % args.idle
            state["idle"] = True
            sock_send(sock, b"QUIT\n")
            state["quit_at"] = now
        if state["quit_at"] is None and args.seconds and now - start > args.seconds:
            state["reason"] = "time limit %d s" % args.seconds
            state["timeout"] = True
            sock_send(sock, b"QUIT\n")
            state["quit_at"] = now
        if state["quit_at"] is not None and now - state["quit_at"] > args.quit_wait:
            status = verdict(state["end"])
            break
        try:
            data = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            data = b""
        if not data:
            status = verdict(state["end"])
            if status == "lost":
                state["reason"] = "the connection closed with no END"
            break
        state["last"] = time.time()
        stream.feed(data, on_line, on_file, on_end, lambda hello: log_file.write("# " + hello + "\n"))
    log_file.close()
    if stream.file is not None:
        stream.file.close()
        result["files"].append(stream.file_name + " (cut)")
    result["status"] = status
    result["reason"] = state["reason"]
    return finish(args, out_dir, result, t0, listener, sock)


def sock_send(sock, data):
    try:
        sock.sendall(data)
    except OSError:
        pass


def finish(args, out_dir, result, t0, listener, sock):
    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass
    if listener is not None:
        listener.close()
    result["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(out_dir, "result.json"), "w") as f:
        json.dump(result, f, indent=2)
    return result


def parse_runs(path):
    runs = []
    base = os.path.dirname(os.path.abspath(path))
    with open(path) as f:
        for no, line in enumerate(f, 1):
            parts = shlex.split(line, comments=True)
            if not parts:
                continue
            if len(parts) < 2:
                sys.exit("%s:%d: a run is `name dol [option ...]`" % (path, no))
            dol = parts[1] if os.path.isabs(parts[1]) else os.path.join(base, parts[1])
            runs.append((parts[0], dol, parts[2:]))
    return runs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wii", required=True, help="the Wii's IP address")
    ap.add_argument("--out", default="netruns", help="output folder (default ./netruns)")
    ap.add_argument("--dol", help="the DOL of a single run")
    ap.add_argument("--name", help="its name (default: the DOL's file name)")
    ap.add_argument("--arg", action="append", default=[], help="an option for the run (repeatable)")
    ap.add_argument("--runs", help="a file of runs: name dol option ...")
    ap.add_argument("--mode", choices=("dial", "listen"), default="dial")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--pc-ip", help="listen mode: the address the Wii reaches this PC at")
    ap.add_argument("--listen-addr", default="0.0.0.0", help="listen mode: the address to bind")
    ap.add_argument("--wiiload", default=os.environ.get("WIILOAD_BIN", "/opt/devkitpro/tools/bin/wiiload"))
    ap.add_argument("--no-load", action="store_true", help="do not run wiiload (the program is already started)")
    ap.add_argument("--fail", default=DEFAULT_FAIL, help="regex of a log line that fails the run ('' for none)")
    ap.add_argument("--fail-grace", type=float, default=15.0, help="seconds of log after a failure line before QUIT")
    ap.add_argument("--end", default="", help="regex of a log line that ends the run (besides END)")
    ap.add_argument("--end-grace", type=float, default=10.0, help="seconds after --end's line before QUIT")
    ap.add_argument("--idle", type=float, default=120.0, help="seconds without a byte before QUIT")
    ap.add_argument("--seconds", type=float, default=0.0, help="time limit of a run (0: none)")
    ap.add_argument("--connect-timeout", type=float, default=90.0)
    ap.add_argument("--quit-wait", type=float, default=30.0, help="seconds to wait for END after QUIT")
    ap.add_argument("--hbc-port", type=int, default=HBC_PORT)
    ap.add_argument("--back-timeout", type=float, default=90.0, help="seconds for the Homebrew Channel to be back")
    ap.add_argument("--no-wait-back", action="store_true")
    ap.add_argument("--echo", action="store_true", help="print the log as it comes")
    args = ap.parse_args(argv)

    if bool(args.dol) == bool(args.runs):
        ap.error("give --dol or --runs")
    runs = parse_runs(args.runs) if args.runs else [
        (args.name or os.path.splitext(os.path.basename(args.dol))[0], os.path.abspath(args.dol), args.arg)]
    os.makedirs(args.out, exist_ok=True)

    def log(text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    worst = 0
    for index, (name, dol, options) in enumerate(runs):
        if not os.path.exists(dol) and not args.no_load:
            log("%s: %s does not exist" % (name, dol))
            worst = 1
            continue
        res = run_one(args, name, dol, options, log)
        log("%s: %s in %.0f s, %d lines, %d files%s" % (name, res["status"], res["seconds"], res["lines"],
                                                         len(res["files"]),
                                                         (" (" + res["reason"] + ")") if res["reason"] else ""))
        if res["status"] != "ok":
            worst = 1
        if index + 1 < len(runs) and not args.no_wait_back and not args.no_load:
            if not wait_hbc(args.wii, args.hbc_port, args.back_timeout):
                log("the Wii did not return to the Homebrew Channel in %d s: stopping (press HOME or reset)"
                    % args.back_timeout)
                return 2
    return worst


if __name__ == "__main__":
    sys.exit(main())
