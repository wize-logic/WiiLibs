#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Runs a Wii DOL/ELF in Dolphin and reports what it did, for tests and for looking.

It gives Dolphin a user folder of its own (the person's settings stay as they are) with the
OSREPORT log written to a file, optionally a host folder synced in as the virtual SD card (sd:/)
and frame dumping, starts Dolphin on the program, waits until a line appears in the log (or a
time limit), captures the render window to a PNG, prints the program's OSREPORT lines and closes
Dolphin. Works from Linux (Dolphin.exe through WSL interop) and on Windows.

Exit status: 0 when --until was seen (or not asked for) and no --fail line appeared; 1 when a
--fail line appeared; 2 when --until never came; 3 when Dolphin could not be run.

  tools/dolphin_run.py build/wii/tests/wl_console_tests.dol --until "WLT DONE" --fail "^FAIL "

Dolphin.exe: --dolphin, else WL_DOLPHIN, else Documents/Emulators/Dolphin-x64/Dolphin.exe in a
Windows profile.

What the program wrote to the card (a log file, a save) is in the run's copy of the card, which Dolphin
writes back when the emulation stops; --sd-out DIR copies out of it what is new or changed since --sd.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ON_WINDOWS = os.name == "nt"

# the program's OSREPORT lines, and what Dolphin says when the GPU's commands or the processor go
# wrong (printed as Dolphin's own lines, when they come)
LOGGER_INI = """[Logs]
OSREPORT = True
OSREPORT_HLE = True
BOOT = True
VIDEO = True
COMMANDPROCESSOR = True
GPFIFO = True
PIXELENGINE = True
POWERPC = True
MASTER = True
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
"""

# PrintWindow on Dolphin's render window (it need not be in front); prints the window title.
CAPTURE_PS1 = r"""
param([string]$Output, [string]$UserFolder)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
# (this run's Dolphin: the one given this run's user folder, not another program's)
$ids = Get-CimInstance Win32_Process -Filter "Name='Dolphin.exe'" | Where-Object { $_.CommandLine -like "*$UserFolder*" } | ForEach-Object { $_.ProcessId }
$process = Get-Process -Id $ids -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$handle = $process.MainWindowHandle
$rect = New-Object Win+RECT
[Win]::GetClientRect($handle, [ref]$rect) | Out-Null
$bitmap = New-Object System.Drawing.Bitmap ([Math]::Max(1, $rect.Right)), ([Math]::Max(1, $rect.Bottom))
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$dc = $graphics.GetHdc()
# PW_CLIENTONLY | PW_RENDERFULLCONTENT: the client area as the GPU drew it
[Win]::PrintWindow($handle, $dc, 3) | Out-Null
$graphics.ReleaseHdc($dc)
$bitmap.Save($Output, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "$($process.MainWindowTitle)"
"""


# The debugger: gdb runs here and reaches Dolphin's GDB stub through a relay on the Windows side
# (stdin/stdout <-> 127.0.0.1:PORT, started over WSL interop's own pipes), so WSL's NAT is no
# obstacle and no port is opened to the network. (The pattern is OpenMMO's mmo/windows/dev.sh.)
RELAY_PS1 = r"""
param([int]$Port)
$c = $null
for ($i = 0; $i -lt 300 -and -not $c; $i++) {
    try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port) } catch { Start-Sleep -Milliseconds 100 }
}
if (-not $c) { [Console]::Error.WriteLine("relay: nothing on 127.0.0.1:$Port"); exit 1 }
$c.NoDelay = $true
$n = $c.GetStream()
$in = [Console]::OpenStandardInput()
$out = [Console]::OpenStandardOutput()
$a = $in.CopyToAsync($n)
$b = $n.CopyToAsync($out)
[void][Threading.Tasks.Task]::WaitAny(@($a, $b))
$c.Close()
"""

GDB_PORT = 55020


def find_gdb():
    """a gdb that knows PowerPC: WL_GDB, else powerpc-eabi-gdb, gdb-multiarch, or a gdb built for
    every target"""
    if os.environ.get("WL_GDB"):
        return os.environ["WL_GDB"]
    for name in ("powerpc-eabi-gdb", "gdb-multiarch"):
        if shutil.which(name):
            return shutil.which(name)
    gdb = shutil.which("gdb")
    if gdb:
        configuration = subprocess.run([gdb, "--configuration"], capture_output=True, text=True).stdout
        if "--enable-targets=all" in configuration:
            return gdb
    return None


def start_gdb(elf: Path, relay: Path, breakpoints=()):
    """gdb attached to Dolphin's stub (which waits for it before running the program), the
    program let go; returns the process, whose stdin takes further commands"""
    gdb = find_gdb()
    if gdb is None:
        print("dolphin_run.py: no gdb that knows PowerPC (WL_GDB, powerpc-eabi-gdb, gdb-multiarch)",
              file=sys.stderr)
        return None
    if ON_WINDOWS:
        remote = f"target remote 127.0.0.1:{GDB_PORT}"
    else:
        ps = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
        remote = (f"target remote | cd /mnt/c && {ps} -NoProfile -ExecutionPolicy Bypass "
                  f"-File '{to_windows(relay)}' {GDB_PORT}")
    process = subprocess.Popen([gdb, "-q", "-nx", "-ex", "set pagination off", "-ex", "set confirm off",
                                "-ex", "set architecture powerpc:750", "-ex", f"file {elf}",
                                # (Dolphin's stub answers the trace status query with a bare OK,
                                # which gdb takes for an error)
                                "-ex", "set remote trace-status-packet off",
                                # (PowerShell takes seconds to start the relay: a shorter timeout
                                # sends the first packet again, Dolphin answers both, and every
                                # reply after is one late)
                                "-ex", "set remotetimeout 60", *(["-ex", "set debug remote 1"] if os.environ.get("WL_GDB_DEBUG") else []), "-ex", remote,
                                *(argument for symbol in breakpoints for argument in ("-ex", f"break {symbol}")),
                                "-ex", "continue"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return process


# Every libogc thread's backtrace: the stub only shows the CPU, which is often the idle thread
# while the program's threads wait. libogc (tuxedo) links its threads from s_firstThread, each
# with the context it was switched out with; each is loaded into the registers in turn for gdb to
# unwind (the program is not resumed afterwards).
THREADS_GDB = """
set $wl_thread = s_firstThread
set $wl_count = 0
while $wl_thread != 0 && $wl_count < 32
  printf "\\n--- thread %p, state %d, priority %d\\n", $wl_thread, $wl_thread->state, $wl_thread->prio
  if $wl_thread->ctx.gpr[1] != 0
    set $pc = $wl_thread->ctx.pc
    set $lr = $wl_thread->ctx.lr
    set $r1 = $wl_thread->ctx.gpr[1]
    bt 24
  end
  set $wl_thread = $wl_thread->next
  set $wl_count = $wl_count + 1
end
"""


def gdb_backtrace(process) -> str:
    """stops the program (gdb's interrupt) and returns where it is: the CPU's backtrace, then
    every libogc thread's"""
    import signal
    process.send_signal(signal.SIGINT)
    time.sleep(1)
    commands = "printf \"--- the CPU\\n\"\nbt 24\ninfo registers pc lr r1\n" + THREADS_GDB + "kill\nquit\n"
    try:
        output, _ = process.communicate(commands, timeout=120)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
    return output


def dolphin_ini(sd_image: str, sd_folder: str, dump_frames: bool, gdb_port: int = 0, video: str = "",
                panics: bool = False, single_core: bool = False, mmu: bool = False,
                unthrottled: bool = False, dump_audio: bool = False) -> str:
    lines = ["[Interface]", "ConfirmStop = False", f"UsePanicHandlers = {panics}", "ShowLogWindow = False",
             "[Analytics]", "PermissionAsked = True", "Enabled = False",
             # (the console's own clock: a person's global Dolphin.ini may overclock the CPU, which
             # would make every timing measured here look faster than the console runs)
             "[Core]", "WiiSDCard = True", "WiiSDCardAllowWrites = True",
             "OverclockEnable = False", "Overclock = 1.0",
             *([f"GFXBackend = {video}"] if video else []),
             *(["CPUThread = False"] if single_core else []),
             # (full MMU emulation: page tables and the DSI a miss raises, which a program that maps
             # memory through the page table and handles its faults needs; Dolphin skips it for speed)
             *(["MMU = True"] if mmu else []),
             # (no speed limit: the console's clock and everything the program sees are the same, the
             # emulated hours only pass sooner where the host has the speed to spare)
             *(["EmulationSpeed = 0.0"] if unthrottled else []),
             f"WiiSDCardEnableFolderSync = {'True' if sd_folder else 'False'}",
             "[General]", f"WiiSDCardPath = {sd_image}"]
    if gdb_port:
        # (Dolphin waits for the debugger to connect before it runs the program)
        lines.append(f"GDBPort = {gdb_port}")
    if sd_folder:
        lines.append(f"WiiSDCardSyncFolder = {sd_folder}")
    if dump_frames:
        lines += ["[Movie]", "DumpFrames = True", "DumpFramesSilent = True"]
    if dump_audio:
        # (the audio the console sent out, as WAV files in User/Dump/Audio)
        lines += ["[DSP]", "DumpAudio = True", "DumpAudioSilent = True"]
    return "\n".join(lines) + "\n"


def gfx_ini(dump_frames: bool, widescreen: bool = False) -> str:
    # CPU access to the EFB on (Dolphin may skip it for speed, and peeks then read 0): programs
    # and tests read pixels back
    text = "[Hacks]\nEFBAccessEnable = True\n"
    # EFB copies to textures written to RAM, as the console does (Dolphin's default keeps them
    # in its own texture cache only and zeroes the RAM they were to go to, so a copy the CPU
    # reads, such as a render target's image, reads 0), and when the copy runs, not deferred to a
    # later synchronization
    text += "EFBToTextureEnable = False\nDeferEFBCopies = False\n"
    # (the window, and so the capture, in the TV's shape: 1 forces 16:9, 2 forces 4:3)
    text += f"[Settings]\nAspectRatio = {1 if widescreen else 2}\n"
    if dump_frames:
        text += "DumpFramesAsImages = True\n"
    return text


def windows_tool(name: str) -> str:
    """a Windows program by name, from System32 when WSL's PATH lacks Windows' directories (as it
    can after a restart, or with appendWindowsPath off)"""
    if ON_WINDOWS or shutil.which(name):
        return name
    for folder in ("/mnt/c/Windows/System32", "/mnt/c/Windows/System32/WindowsPowerShell/v1.0"):
        candidate = Path(folder) / name
        if candidate.exists():
            return str(candidate)
    return name


def to_windows(path: Path) -> str:
    if ON_WINDOWS:
        return str(path)
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True,
                          check=True).stdout.strip()


def windows_temp() -> Path:
    if ON_WINDOWS:
        return Path(os.environ["TEMP"])
    temp = subprocess.run([windows_tool("cmd.exe"), "/c", "echo %TEMP%"], capture_output=True, text=True,
                          cwd="/mnt/c").stdout.strip()
    return Path(subprocess.run(["wslpath", "-u", temp], capture_output=True, text=True,
                               check=True).stdout.strip())


def find_dolphin(explicit) -> Path:
    if explicit:
        return Path(explicit)
    if os.environ.get("WL_DOLPHIN"):
        return Path(os.environ["WL_DOLPHIN"])
    users = Path("C:/Users") if ON_WINDOWS else Path("/mnt/c/Users")
    candidates = sorted(users.glob("*/Documents/Emulators/Dolphin-x64/Dolphin.exe"))
    if not candidates:
        sys.exit("dolphin_run.py: no Dolphin.exe; pass --dolphin or set WL_DOLPHIN")
    return candidates[0]


def copy_tree(source: Path, destination: Path) -> None:
    """contents only: a Windows drive mounted in WSL refuses the permission bits copytree copies, and the setting of
    a file's times. A record beside the copy (<destination>.copied.json) keeps, for each file copied, its source's size
    and time and the copy's own time just after it was written. A file whose source and copy both still match is kept,
    so a large card (a game's data) is copied once, not at every run; what the source lacks is removed. A file a run
    wrote (Dolphin syncs the card back into the copy when it stops) has another time and is copied again, so what an
    earlier run wrote, or left half written, never reaches the next run"""
    record_path = destination.with_name(destination.name + ".copied.json")
    try:
        record = json.loads(record_path.read_text())
    except (FileNotFoundError, ValueError):
        record = {}
    copied = {}
    wanted = set()
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        wanted.add(relative)
        target = destination / relative
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        status = path.stat()
        key = relative.as_posix()
        try:
            present = target.stat()
            if record.get(key) == [status.st_size, status.st_mtime_ns, present.st_mtime_ns]:
                copied[key] = record[key]
                continue
        except FileNotFoundError:
            pass
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        copied[key] = [status.st_size, status.st_mtime_ns, target.stat().st_mtime_ns]
    for path in sorted(destination.rglob("*"), reverse=True):
        if path.relative_to(destination) not in wanted:
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            else:
                path.unlink(missing_ok=True)
    record_path.write_text(json.dumps(copied))


SMALL_FILE = 1 << 20


def card_changes(source: Path, card: Path):
    """the files of the card (a folder) that are not in source (a folder) or differ from its: a different
    size, or for a file of 1 MiB or less different bytes. A larger file of the same size counts as the same
    (a game's data is not read twice at every run; Dolphin may write every file back with a new time, so
    times say nothing). Relative paths, in order."""
    changed = []
    for path in sorted(card.rglob("*")):
        if path.is_dir():
            continue
        relative = path.relative_to(card)
        original = source / relative
        if not original.is_file():
            changed.append(relative)
            continue
        size = path.stat().st_size
        if size != original.stat().st_size:
            changed.append(relative)
        elif size <= SMALL_FILE and path.read_bytes() != original.read_bytes():
            changed.append(relative)
    return changed


def copy_out(source: Path, card: Path, destination: Path):
    """what the program changed on the card, copied to destination (the folders it is in made), nothing else
    removed or overwritten but the files themselves; the relative paths copied"""
    changed = card_changes(source, card)
    for relative in changed:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(card / relative, target)
    return changed


def ours(user_folder: str) -> str:
    """PowerShell for this run's Dolphin processes: those given this run's user folder (--name),
    so that runs of other programs, or other people's Dolphins, are left alone"""
    escaped = user_folder.replace("'", "''")
    return ("Get-CimInstance Win32_Process -Filter \"Name='Dolphin.exe'\" | "
            f"Where-Object {{ $_.CommandLine -like '*{escaped}*' }}")


def kill_dolphin(user_folder: str) -> None:
    subprocess.run([windows_tool("powershell.exe"), "-NoProfile", "-Command",
                    f"{ours(user_folder)} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"],
                   capture_output=True)


def close_dolphin(user_folder: str, seconds: float = 15) -> None:
    """stops the emulation as closing its window does (ConfirmStop is off), so that Dolphin writes
    the SD card's folder back; killed if it has not gone in time"""
    subprocess.run([windows_tool("powershell.exe"), "-NoProfile", "-Command",
                    f"{ours(user_folder)} | ForEach-Object {{ (Get-Process -Id $_.ProcessId).CloseMainWindow() | Out-Null }}"],
                   capture_output=True)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        running = subprocess.run([windows_tool("powershell.exe"), "-NoProfile", "-Command",
                                  f"@({ours(user_folder)}).Count"], capture_output=True, text=True)
        if running.stdout.strip() in ("0", ""):
            return
        time.sleep(0.5)
    kill_dolphin(user_folder)


def dolphin_messages(log: Path, most: int = 24):
    """Dolphin's own warnings and errors, each kind once with how many times it came (an invalid
    access in a loop logs millions): the first of each, its numbers and addresses aside"""
    if not log.exists():
        return []
    kinds = {}
    order = []
    with log.open(errors="replace") as lines:
        for line in lines:
            if "OSREPORT" in line or ("E[" not in line and "W[" not in line):
                continue
            line = line.rstrip()
            text = line.split(": ", 1)[-1] if " W[" in line or " E[" in line else line
            kind = re.sub(r"0x[0-9a-fA-F]+|\b\d+\b", "#", text)
            if kind not in kinds:
                kinds[kind] = [line, 0]
                order.append(kind)
            kinds[kind][1] += 1
    messages = []
    for kind in order[:most]:
        first, count = kinds[kind]
        messages.append(first if count == 1 else f"{first} (and {count - 1} more like it)")
    if len(order) > most:
        messages.append(f"... {len(order) - most} more kinds of message")
    return messages


class RunWatch:
    """when a run ends early: a --fail line ends it grace seconds later (what the program writes after it, a freeze
    watch's profile, still reaches the log); no new line for idle seconds ends it at once (a hang that stops the
    program's own watchers too). update() is given the log's lines and the time, and says why it changed course"""

    def __init__(self, fail, grace: float, idle: float, started: float, deadline: float):
        self.fail, self.grace, self.idle = fail, grace, idle
        self.started, self.deadline = started, deadline
        self.failed = self.idle_ended = False
        self.count, self.changed = 0, started

    def update(self, lines, now: float) -> str:
        if len(lines) != self.count:
            self.count, self.changed = len(lines), now
        if not self.failed and self.fail is not None and any(self.fail.search(line) for line in lines):
            self.failed = True
            self.deadline = min(self.deadline, now + self.grace)
            return f"a --fail line after {now - self.started:.0f} s; ending the run in {self.grace:g} s"
        if self.idle and now - self.changed > self.idle:
            self.failed = self.idle_ended = True
            return f"no new OSREPORT line for {self.idle:g} s; ending the run"
        return ""


def osreport_lines(log: Path):
    if not log.exists():
        return []
    return [line.split("]: ", 1)[-1] for line in log.read_text(errors="replace").splitlines()
            if "OSREPORT" in line]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("program", type=Path, help="the .dol or .elf to boot")
    parser.add_argument("--dolphin", help="Dolphin.exe (default: WL_DOLPHIN, else Documents/Emulators/Dolphin-x64)")
    parser.add_argument("--seconds", type=float, default=30, help="the longest to wait (default 30)")
    parser.add_argument("--until", help="stop once an OSREPORT line contains this text")
    parser.add_argument("--fail", help="a regular expression; an OSREPORT line matching it fails the run and ends it "
                                       "--fail-grace seconds later (a recompiled title's freeze, stall and crash "
                                       "reports: 'freeze: no new frame|stall: no scheduling|GUEST CRASH|\\*\\*\\* CRASH')")
    parser.add_argument("--fail-grace", type=float, default=15,
                        help="seconds a run goes on after a --fail line, so what the program writes after it (a "
                             "freeze watch's 10 s profile) reaches the log (default 15)")
    parser.add_argument("--idle", type=float, default=0,
                        help="end the run, failed, when no new OSREPORT line comes for this many seconds (a hang that "
                             "stops the program's own watchers too); 0, the default, never")
    parser.add_argument("--sd", type=Path, help="a folder synced in as the virtual SD card (sd:/)")
    parser.add_argument("--sd-out", type=Path, metavar="DIR",
                        help="after the run, copy here what the program wrote to the card (files new or changed "
                             "since --sd, with their folders): a log, a save")
    parser.add_argument("--screenshot", type=Path, help="save the render window here (PNG)")
    parser.add_argument("--single-core", action="store_true",
                        help="the GPU emulated on the CPU's thread (slower; no CPU/GPU races)")
    parser.add_argument("--mmu", action="store_true",
                        help="Dolphin's full MMU emulation (page tables, and a DSI on a miss)")
    parser.add_argument("--dump-audio", type=Path, metavar="DIR",
                        help="copy the audio the console sent out (Dolphin's WAV dumps) into this folder")
    parser.add_argument("--unthrottled", action="store_true",
                        help="no speed limit: a long run finishes sooner, the program sees no difference")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the run's copies (the card's folder and image, the program, the dumps) in its "
                             "work folder; by default they are deleted when the run ends, keeping the logs")
    parser.add_argument("--panics", action="store_true",
                        help="show Dolphin's panic messages (they stop the emulation until answered)")
    parser.add_argument("--wii-aspect", choices=("4:3", "16:9"), default="4:3",
                        help="the TV's shape the console's settings give programs (default 4:3)")
    parser.add_argument("--wii-progressive", action="store_true",
                        help="the console's 480p setting on (SYSCONF IPL.PGS; Dolphin always reports a component cable)")
    parser.add_argument("--wii-eurgb60", choices=("on", "off"), default=None,
                        help="the console's EuRGB60 setting (SYSCONF IPL.E60); it matters only in a PAL region "
                             "(a PAL NAND: Dolphin's region setting), where off gives 576i at 50 Hz")
    parser.add_argument("--video", default="Vulkan",
                        help="Dolphin's video backend (Vulkan, D3D, D3D12, OGL, or '' for Dolphin's own; default "
                             "Vulkan: long runs on Direct3D 11 have reset the GPU's driver)")
    parser.add_argument("--dump-frames", type=Path, help="dump every frame as PNGs into this folder")
    parser.add_argument("--name", default="wl-dolphin", help="the work folder's name under %%TEMP%%")
    parser.add_argument("--break", dest="break_at", action="append", default=[], metavar="SYMBOL",
                        help="with --backtrace: stop there (exit, abort, a function); the backtrace is then "
                             "of where it stopped")
    parser.add_argument("--backtrace", type=Path, metavar="ELF",
                        help="run under gdb, through Dolphin's GDB stub, with this ELF's symbols; at the end, stop "
                             "the program and print its backtrace and registers")
    args = parser.parse_args()
    if args.sd_out and not args.sd:
        parser.error("--sd-out needs --sd (the card it copies the changes of)")

    dolphin = find_dolphin(args.dolphin)
    work = windows_temp() / args.name
    user = work / "user"
    if user.exists():
        shutil.rmtree(user / "Dump", ignore_errors=True)
    (user / "Config").mkdir(parents=True, exist_ok=True)
    sd_folder = ""
    if args.sd:
        # Dolphin builds the card from the folder at boot; a copy beside it keeps the source safe
        # from the sync back
        synced = work / "sd"
        synced.mkdir(parents=True, exist_ok=True)
        copy_tree(args.sd, synced)
        sd_folder = to_windows(synced)
    (user / "Config" / "Logger.ini").write_text(LOGGER_INI)
    (user / "Config" / "Dolphin.ini").write_text(dolphin_ini(to_windows(work / "sd.raw"), sd_folder,
                                                              bool(args.dump_frames),
                                                              GDB_PORT if args.backtrace else 0, args.video,
                                                              args.panics, args.single_core, args.mmu,
                                                              args.unthrottled, bool(args.dump_audio)))
    (user / "Config" / "GFX.ini").write_text(gfx_ini(bool(args.dump_frames), args.wii_aspect == "16:9"))
    log = user / "Logs" / "dolphin.log"
    if log.exists():
        log.unlink()
    program = work / args.program.name
    shutil.copyfile(args.program, program)
    (work / "capture.ps1").write_text(CAPTURE_PS1)

    kill_dolphin(to_windows(user))
    try:
        process = subprocess.Popen([str(dolphin), "-u", to_windows(user), "-b",
                                    # (the console's TV shape, SYSCONF's IPL.AR, which programs read)
                                    "-C", f"SYSCONF.IPL.AR={1 if args.wii_aspect == '16:9' else 0}",
                                    *(["-C", "SYSCONF.IPL.PGS=1"] if args.wii_progressive else []),
                                    *(["-C", f"SYSCONF.IPL.E60={1 if args.wii_eurgb60 == 'on' else 0}"]
                                      if args.wii_eurgb60 else []),
                                    "-e", to_windows(program)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as error:
        print(f"dolphin_run.py: {error}", file=sys.stderr)
        return 3
    debugger = None
    if args.backtrace:
        (work / "relay.ps1").write_text(RELAY_PS1)
        debugger = start_gdb(args.backtrace, work / "relay.ps1", args.break_at)
    fail = re.compile(args.fail) if args.fail else None
    seen_until = args.until is None
    failed = False
    started = time.monotonic()
    deadline = started + args.seconds
    watch = RunWatch(fail, args.fail_grace, args.idle, started, deadline)
    while time.monotonic() < watch.deadline and process.poll() is None:
        lines = osreport_lines(log)
        note = watch.update(lines, time.monotonic())
        if note:
            print(f"dolphin_run.py: {note}", file=sys.stderr)
        failed = watch.failed
        if watch.idle_ended:
            break
        if args.until and any(args.until in line for line in lines):
            seen_until = True
            time.sleep(2)  # the frame after it reaches the screen
            break
        time.sleep(1)

    exited = process.poll() is not None
    if exited and not seen_until:
        print(f"dolphin_run.py: Dolphin exited by itself after {time.monotonic() - started:.0f} s (the program "
              "stopped or reset, or crashed into a panic screen that reloads, or Dolphin failed)", file=sys.stderr)
    if debugger is not None:
        print("backtrace:")
        for line in gdb_backtrace(debugger).splitlines():
            if line.strip() and not line.startswith(("warning:", "Type ", "For help")):
                print(f"  {line}")
    if args.screenshot:
        capture = work / "capture.png"
        title = subprocess.run([windows_tool("powershell.exe"), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                to_windows(work / "capture.ps1"), to_windows(capture), to_windows(user)],
                               capture_output=True, text=True)
        if capture.exists():
            args.screenshot.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(capture), args.screenshot)
            print(f"screenshot: {args.screenshot} ({title.stdout.strip()})")
        else:
            print(f"no screenshot: {title.stderr.strip()}", file=sys.stderr)
    close_dolphin(to_windows(user))
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
    if args.sd_out:
        # (Dolphin wrote the card back to the run's copy as it stopped)
        copied = copy_out(args.sd, work / "sd", args.sd_out)
        print(f"sd out: {len(copied)} files in {args.sd_out}" + "".join(f"\n  {relative}" for relative in copied[:20]))
    if args.dump_frames:
        frames = sorted((user / "Dump" / "Frames").glob("*.png"))
        args.dump_frames.mkdir(parents=True, exist_ok=True)
        for frame in frames:
            try:
                shutil.copyfile(frame, args.dump_frames / frame.name)
            except FileNotFoundError:  # (removed under us: another run with the same --name started)
                pass
        print(f"frames: {len(frames)} in {args.dump_frames}")
    if args.dump_audio:
        waves = sorted((user / "Dump" / "Audio").glob("*.wav"))
        args.dump_audio.mkdir(parents=True, exist_ok=True)
        for wave in waves:
            shutil.copyfile(wave, args.dump_audio / wave.name)
        print(f"audio: {len(waves)} files in {args.dump_audio}")
    if not args.keep_work:
        # the run's copies (the card's folder, its image, the program, the dumps) are gigabytes per
        # run name and are made again by the next run; the logs and the configuration stay
        for path in (work / "sd", work / "sd.copied.json", work / "sd.raw", program, user / "Dump"):
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            elif path.exists():
                path.unlink(missing_ok=True)

    lines = osreport_lines(log)
    for line in lines:
        print(line)
    for line in dolphin_messages(log):
        print(f"dolphin: {line}")
    if fail is not None and any(fail.search(line) for line in lines):
        failed = True
    if failed:
        return 1
    if not seen_until:
        print(f"dolphin_run.py: '{args.until}' never appeared in {args.seconds:g} s", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
