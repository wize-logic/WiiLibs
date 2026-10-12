#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/xboxrecomp_sync.py against a synthetic upstream git repository.

An upstream with a lifter, a tool package, an LGPL file listed in its NOTICE and a README changelog
is vendored into a scratch WiiLibs tree with one patch; then upstream moves on (an edit, a rename, a
new LGPL file, an unmapped file, a release), and the tool must brief it, refresh the pristine copy,
re-apply the patch, refuse what it must refuse and record the triage. Needs git.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SYNC = HERE.parent.parent / "tools" / "xboxrecomp_sync.py"

LIFTER = "".join(f"line {i}\n" for i in range(1, 41))
README1 = """# up

## Changelog

### v0.1.0 — *"First"* (October 2026)

The first one.

- **Lifter**: lifts things
"""
README2 = README1.replace("## Changelog\n", """## Changelog

### v0.2.0 — *"Second"* (October 2026)

Renames and a new audio file.

- **Disasm**: core.py replaces a.py
""")


def sh(cwd, *args, check=True):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CEILING_DIRECTORIES=str(Path(cwd).parent))
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise AssertionError(f"{args} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r


def write(root, rel, text):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


MAP = {
    "schema": 1,
    "upstream": {"name": "up", "url": "file:///nonexistent", "branch": "main", "license": "MIT"},
    "vendor_dir": "third_party/xboxrecomp",
    "areas": [{"path": "NOTICE", "area": "meta"}, {"path": "LICENSE", "area": "meta"},
              {"path": "README.md", "area": "docs"}, {"path": "tools/recomp/**", "area": "lifter"},
              {"path": "tools/disasm/**", "area": "analysis"}, {"path": "src/apu/**", "area": "audio"}],
    "rules": [{"upstream": "tools/recomp/lifter.py", "kind": "patched", "wl": ["third_party/xboxrecomp/patches/"],
               "wl_tests": ["xboxrecomp_pytest"]},
              {"upstream": "tools/**", "kind": "vendored"},
              {"upstream": "LICENSE", "kind": "vendored"},
              {"upstream": "NOTICE", "kind": "concept"},
              {"upstream": "README.md", "kind": "concept"},
              {"upstream": "src/apu/**", "kind": "concept", "license": "LGPL (NOTICE)",
               "wl": ["wl/xboxrt (planned)"], "status": "planned"}],
}


class SyncTest(unittest.TestCase):
    def setUp(self):
        self.td = Path(tempfile.mkdtemp(prefix="wl-sync-test-"))
        self.up = self.td / "up"
        self.wl = self.td / "wl"
        self.up.mkdir()
        sh(self.up, "git", "init", "-q", "-b", "main")
        write(self.up, "tools/__init__.py", "")
        write(self.up, "tools/recomp/lifter.py", LIFTER)
        write(self.up, "tools/disasm/a.py", "".join(f"a {i}\n" for i in range(30)))
        write(self.up, "LICENSE", "MIT License\n\nCopyright (c) 2026 someone\n")
        write(self.up, "NOTICE", "LGPL-2.1-or-later\n\n    src/apu/old.c    thing\n")
        write(self.up, "src/apu/old.c", "/* LGPL v2.1 or later */\n")
        write(self.up, "README.md", README1)
        sh(self.up, "git", "add", "-A")
        sh(self.up, "git", "commit", "-qm", "Release v0.1.0 -- \"First\"")
        self.c1 = sh(self.up, "git", "rev-parse", "HEAD").stdout.strip()
        vend = self.wl / "third_party" / "xboxrecomp"
        (vend / "patches").mkdir(parents=True)
        (vend / "upstream_map.json").write_text(json.dumps(MAP, indent=1))
        # a patch made the way the docs say: a git repository of the file, one commit
        work = self.td / "patchwork"
        write(work, "tools/recomp/lifter.py", LIFTER)
        sh(work, "git", "init", "-q", "-b", "main")
        sh(work, "git", "add", "-A")
        sh(work, "git", "commit", "-qm", "base")
        write(work, "tools/recomp/lifter.py", LIFTER.replace("line 5\n", "line 5 WiiLibs\n"))
        sh(work, "git", "commit", "-qam", "recomp: change line 5\n\nWhy it is needed.")
        sh(work, "git", "format-patch", "-q", "-N", "--zero-commit", "-o", str(vend / "patches"), "HEAD~1")
        p = next((vend / "patches").glob("*.patch"))
        p.rename(vend / "patches" / "0001-line-five.patch")

    def tearDown(self):
        shutil.rmtree(self.td, ignore_errors=True)

    def sync(self, *args, check=True):
        r = subprocess.run([sys.executable, str(SYNC), "--wl-root", str(self.wl), "--repo", str(self.up), *args],
                           capture_output=True, text=True)
        if check and r.returncode != 0:
            raise AssertionError(f"sync {args} exited {r.returncode}:\n{r.stdout}\n{r.stderr}")
        return r

    def state(self):
        return json.loads((self.wl / "third_party/xboxrecomp/UPSTREAM.json").read_text())

    def upstream_moves_on(self):
        write(self.up, "tools/recomp/lifter.py", LIFTER.replace("line 30\n", "line 30 upstream\n"))
        sh(self.up, "git", "mv", "tools/disasm/a.py", "tools/disasm/core.py")
        write(self.up, "tools/disasm/core.py", "".join(f"a {i}\n" for i in range(30)).replace("a 20\n", "a 20 b\n"))
        write(self.up, "src/apu/new.c", "/* Copyright (c) someone else\n * GNU Lesser General Public License, extracted from xemu */\n")
        write(self.up, "extra/thing.txt", "unmapped\n")
        write(self.up, "README.md", README2)
        sh(self.up, "git", "add", "-A")
        sh(self.up, "git", "commit", "-qm", "disasm: rename a to core (#7)")
        sh(self.up, "git", "commit", "-q", "--allow-empty", "-m", "Release v0.2.0 -- \"Second\"")
        return sh(self.up, "git", "rev-parse", "HEAD").stdout.strip()

    def test_full_cycle(self):
        vend = self.wl / "third_party" / "xboxrecomp"
        # first vendoring: a dry run writes nothing, --apply makes the pristine copy and the state
        out = self.sync("merge", "--to", self.c1).stdout
        self.assertIn("dry run", out)
        self.assertFalse((vend / "UPSTREAM.json").exists())
        out = self.sync("merge", "--to", self.c1, "--apply").stdout
        self.assertIn("0001-line-five.patch: applies", out)
        st = self.state()
        self.assertEqual(st["vendored"]["commit"], self.c1)
        self.assertEqual(st["vendored"]["release"], "v0.1.0")
        self.assertEqual(st["triaged"]["commit"], self.c1)
        self.assertEqual(sorted(st["files"]), ["LICENSE", "tools/__init__.py", "tools/disasm/a.py",
                                               "tools/recomp/lifter.py"])
        self.assertEqual((vend / "tools/recomp/lifter.py").read_text(), LIFTER)   # pristine
        self.assertFalse((vend / "src").exists())                                   # concept: not copied
        self.assertEqual(self.sync("status").returncode, 0)

        # an edit to the copy is caught; the copy must stay pristine
        (vend / "tools/disasm/a.py").write_text("edited\n")
        r = self.sync("status", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("modified: tools/disasm/a.py", r.stdout)
        r = self.sync("merge", "--to", self.c1, "--apply", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("was edited", r.stderr)
        sh(self.up, "git", "show", f"{self.c1}:tools/disasm/a.py")
        (vend / "tools/disasm/a.py").write_text("".join(f"a {i}\n" for i in range(30)))
        self.assertEqual(self.sync("status").returncode, 0)

        c2 = self.upstream_moves_on()

        # the brief: release notes, areas, licence flags, mapping health, the patch against c2
        brief = self.sync("brief").stdout
        self.assertIn(f"# xboxrecomp upstream brief: {self.c1[:7]}..{c2[:7]}", brief)
        self.assertIn('### v0.2.0 "Second"', brief)
        self.assertIn("## Area: lifter", brief)
        self.assertIn("## Area: analysis", brief)
        self.assertIn("| BLOCK | `src/apu/new.c` |", brief)
        self.assertIn("R", brief)
        self.assertIn("`tools/disasm/a.py` -> `tools/disasm/core.py`", brief)
        self.assertIn("`extra/thing.txt` (area other): decide", brief)
        self.assertIn("| `0001-line-five.patch` | yes | `tools/recomp/lifter.py` |", brief)
        self.assertIn("third_party/xboxrecomp/patches/", brief)
        self.assertIn("- [ ] ", brief)
        split = self.td / "split"
        self.sync("brief", "--split-dir", str(split))
        names = sorted(p.name for p in split.iterdir())
        self.assertIn("brief_lifter.md", names)
        self.assertIn("brief_audio.md", names)
        self.assertIn("Licence flags", (split / "brief_audio.md").read_text())
        self.assertIn("This file is the brief for one area, **lifter**", (split / "brief_lifter.md").read_text())

        # advancing before the merge is refused
        r = self.sync("advance", "--to", c2, check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("merge --to", r.stderr)

        # the merge: a fresh pristine copy, the rename followed, the patch still applies
        out = self.sync("merge", "--apply").stdout
        self.assertIn("A tools/disasm/core.py", out)
        self.assertIn("D tools/disasm/a.py", out)
        self.assertIn("0001-line-five.patch: applies", out)
        self.assertTrue((vend / "tools/disasm/core.py").exists())
        self.assertFalse((vend / "tools/disasm/a.py").exists())
        self.assertIn("line 30 upstream", (vend / "tools/recomp/lifter.py").read_text())
        self.assertNotIn("WiiLibs", (vend / "tools/recomp/lifter.py").read_text())
        self.assertEqual(self.state()["vendored"]["commit"], c2)
        self.assertEqual(self.state()["triaged"]["commit"], self.c1)

        # check-map: the unmapped file is a problem until a rule covers it
        r = self.sync("check-map", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("unmapped upstream file: extra/thing.txt", r.stdout)
        mp = json.loads((vend / "upstream_map.json").read_text())
        mp["rules"].append({"upstream": "extra/**", "kind": "ignore"})
        (vend / "upstream_map.json").write_text(json.dumps(mp, indent=1))
        r = self.sync("check-map", check=False)
        self.assertEqual(r.returncode, 0, r.stdout)

        # advance records the triage
        self.sync("advance", "--to", c2, "--note", "reviewed")
        st = self.state()
        self.assertEqual(st["triaged"]["commit"], c2)
        self.assertEqual(st["triaged"]["release"], "v0.2.0")
        self.assertEqual(st["history"][0]["commit"], self.c1)

        # upstream changes the line the patch changes: the merge reports it and advance refuses
        write(self.up, "tools/recomp/lifter.py", LIFTER.replace("line 30\n", "line 30 upstream\n")
              .replace("line 5\n", "line 5 upstream\n"))
        sh(self.up, "git", "commit", "-qam", "recomp: line 5")
        c3 = sh(self.up, "git", "rev-parse", "HEAD").stdout.strip()
        r = self.sync("merge", "--apply", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("0001-line-five.patch: DOES NOT APPLY", r.stdout)
        r = self.sync("advance", "--to", c3, check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("patches do not apply", r.stderr)

    def test_lgpl_file_mapped_vendored_is_refused(self):
        mp = dict(MAP)
        mp["rules"] = [{"upstream": "src/apu/**", "kind": "vendored"}] + MAP["rules"]
        (self.wl / "third_party/xboxrecomp/upstream_map.json").write_text(json.dumps(mp))
        r = self.sync("merge", "--to", self.c1, "--apply", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("REFUSED", r.stdout)
        self.assertIn("src/apu/old.c", r.stdout)

    def test_patch_repo_round_trip(self):
        self.sync("merge", "--to", self.c1, "--apply")
        repo = self.td / "pr"
        self.sync("patch-repo", str(repo))
        self.assertIn("line 5 WiiLibs", (repo / "tools/recomp/lifter.py").read_text())
        log = sh(repo, "git", "log", "--format=%s").stdout.split("\n")
        self.assertEqual(log[0], "recomp: change line 5")
        # amend the patch there and export it back: same name, new content
        write(repo, "tools/recomp/lifter.py", LIFTER.replace("line 5\n", "line 5 WiiLibs v2\n"))
        sh(repo, "git", "commit", "-q", "-a", "--amend", "--no-edit")
        self.sync("export-patches", str(repo))
        patches = sorted((self.wl / "third_party/xboxrecomp/patches").glob("*.patch"))
        self.assertEqual([p.name for p in patches], ["0001-line-five.patch"])
        self.assertIn("line 5 WiiLibs v2", patches[0].read_text())
        self.assertEqual(self.sync("status").returncode, 0)

    def test_fetch_into_the_cache_clone(self):
        def sync_cache(*args, check=True):
            r = subprocess.run([sys.executable, str(SYNC), "--wl-root", str(self.wl), *args],
                               capture_output=True, text=True)
            if check and r.returncode != 0:
                raise AssertionError(f"sync {args} exited {r.returncode}:\n{r.stdout}\n{r.stderr}")
            return r
        r = sync_cache("status", check=False)          # no state yet
        self.assertEqual(r.returncode, 1)
        out = sync_cache("fetch", "--url", str(self.up)).stdout
        self.assertIn("cloned", out)
        cache = self.wl / "build" / "xboxrecomp_sync" / "upstream.git"
        self.assertTrue(cache.is_dir())
        sync_cache("merge", "--to", self.c1, "--apply")
        c2 = self.upstream_moves_on()
        out = sync_cache("fetch", "--url", str(self.up)).stdout
        self.assertIn(c2, out)
        out = sync_cache("status").stdout
        self.assertIn("behind by 2 commit(s), 1 release(s)", out)
        # the synced commit is pinned so a force-push upstream cannot lose it
        refs = sh(cache, "git", "for-each-ref", "refs/wl-sync").stdout
        self.assertIn(self.c1, refs)

    def test_test_command_dry_run(self):
        self.sync("merge", "--to", self.c1, "--apply")
        out = self.sync("test", "--dry-run", "--wii").stdout
        self.assertIn("host: ctest", out)
        self.assertIn("ppc-be: ctest", out)
        self.assertIn("wii: build", out)


if __name__ == "__main__":
    if not shutil.which("git"):
        print("skipped: git not found")
        sys.exit(77)
    unittest.main()
