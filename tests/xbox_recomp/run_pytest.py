#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""xboxrecomp's own pytest suite, on the patched copy WiiLibs builds with.

The vendored pipeline is copied into the build tree and WiiLibs' patches applied
(tools/xbox_recomp.py prepare), then upstream's tests and the patches' tests run there. Tests that
need files WiiLibs does not vendor (upstream's C runtime in src/) are deselected, each with its
reason below; everything else must pass.

    run_pytest.py --build DIR [pytest arguments...]

Python: the first of $WL_PYTEST_PYTHON, this interpreter, python3 on PATH and /usr/bin/python3 that
has pytest and capstone. Exit status: pytest's, or 77 when no Python has both.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

WL_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WL_ROOT / "tools"))
import xbox_recomp  # noqa: E402

SKIP = 77
# node id -> why it cannot run against the vendored subset
DESELECT = {
    "tools/kernel_audit/test_bridge_ordinals.py": "reads upstream's src/kernel/kernel_bridge.c (the Win32 "
                                                  "runtime, not vendored)",
    "tools/recomp/test_icall_guarded_runtime.py::test_runtime_site_dump_round_trips_through_the_merge_parser":
        "compiles upstream's src/kernel/icall_feedback.c (not vendored)",
    "tools/recomp/test_lifter_mxcsr.py::MxcsrLiftTest::test_runtime_declares_the_state":
        "reads upstream's src/kernel/xbox_memory_layout.c (not vendored); WiiLibs' g_mxcsr is in "
        "wl/xboxrt/recomp_symbols.inc",
}


def find_python() -> str | None:
    cands = [os.environ.get("WL_PYTEST_PYTHON"), sys.executable, shutil.which("python3"), "/usr/bin/python3"]
    for c in cands:
        if c and os.path.exists(c):
            # (from /, and asking for pytest.main: in the test's own directory the work folder
            # "pytest" imports as an empty namespace package, which a bare import accepts)
            r = subprocess.run([c, "-c", "import pytest, capstone; pytest.main"], capture_output=True, cwd="/")
            if r.returncode == 0:
                return c
    return None


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) < 2 or argv[0] != "--build":
        print(__doc__)
        return 2
    work = Path(argv[1]).resolve()
    extra = argv[2:]
    py = find_python()
    if not py:
        print("skipped: no Python with pytest and capstone (pip install pytest capstone, or set WL_PYTEST_PYTHON)")
        return SKIP
    pipe = xbox_recomp.prepare_pipeline(work / "pipeline", quiet=True)
    cmd = [py, "-m", "pytest", "-q", "-p", "no:cacheprovider", "tools"]
    for node, why in DESELECT.items():
        if "::" in node:
            cmd += ["--deselect", node]
        else:
            cmd += ["--ignore", node]
        print(f"deselected {node}: {why}")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    return subprocess.run(cmd + extra, cwd=pipe, env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
