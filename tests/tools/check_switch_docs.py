#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The runtime's switches against their documents. wl/xboxrt/README.md's tables ("Files in the title
folder", "Off switches", the environment variables and the build-time switches) against the code: every
title-folder switch the runtime reads (wl/xbox/nv2a_gx.c's k_switches, boot.c's title_flag and flag paths,
its k_compat, memfast_wii.c's oldcopy) and every WL_XB_* variable it or the replay reads has a row; every
switch and variable a row of "Off switches" names is still read; every halo.json key of the build-time
table is one tools/xbox_recomp.py reads. And third_party/xboxrecomp/patches numbered 0001 on without a gap,
no .pyc among them. A new switch fails this test until its row is written."""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
readme = (root / 'wl/xboxrt/README.md').read_text()

code = ''
for f in list((root / 'wl').rglob('*.c')) + [root / 'tools/xbox_nv2a_replay.c']:
    code += f.read_text(errors='replace')

files = set()
files |= set(re.findall(r'\{"([a-z0-9]+)", "WL_XB_[A-Z0-9_]+", SWITCH_', code))  # nv2a_gx.c k_switches
files |= set(re.findall(r'title_flag\(cfg, "([a-z0-9]+)"', code))  # boot.c gx_bisect_options & co.
files |= set(re.findall(r'"%s/([a-z0-9]+)"', code))  # boot.c, memfast_wii.c flag paths
files |= set(re.findall(r'\{"(old[a-z]+)", (?:WL_XB_COMPAT_[A-Z]+|0u)\}', code))  # boot.c k_compat
not_switches = {'hdd', 'logs', 'play', 'shots', 'files', 'pagefile', 'recomp_seeds', 'vsh_programs', 'netrun'}
files -= not_switches
envs = set(re.findall(r'"(WL_XB_[A-Z0-9_]+)"', code))

# the README's tables: rows of the "Files in the title folder", "Off switches" and variables tables
rows = [l for l in readme.split('\n') if l.startswith('| `')]
first_cells = ' '.join(l.split(' | ')[0] for l in rows)
named_files = set(re.findall(r'`([a-z0-9]+)`', first_cells))
row_text = '\n'.join(rows)
named_envs = {e for e in re.findall(r'`(WL_XB_[A-Z0-9_]+)', row_text) if not e.startswith('WL_XB_COMPAT_')}

bad = 0
for s in sorted(files - named_files):
    print('undocumented switch file:', s)
    bad += 1
for e in sorted(envs - named_envs):
    print('undocumented variable:', e)
    bad += 1
# rows of the off switches' table naming a file nothing reads
start = readme.index('**Off switches.**')
end = readme.index('**Build-time switches**')
for l in readme[start:end].split('\n'):
    if l.startswith('| `'):
        for s in re.findall(r'`([a-z0-9]+)`', l.split(' | ')[0]):
            if s not in files:
                print('row names a switch nothing reads:', s)
                bad += 1
        for e in re.findall(r'`(WL_XB_[A-Z0-9_]+)', l.split(' | ')[1]):
            if e not in envs:
                print('row names a variable nothing reads:', e)
                bad += 1
for e in sorted(named_envs - envs):
    print('README names a variable nothing reads:', e)
    bad += 1
# halo.json's build-time keys
recomp = (root / 'tools/xbox_recomp.py').read_text()
for k in re.findall(r'^\| `([a-z_]+)` \| wave', readme[end:], re.M):
    if f'c.get("{k}"' not in recomp:
        print('build-time key nothing reads:', k)
        bad += 1
# the recompiler's patch files: 0001-... on, numbered without a gap, nothing else among them
patches = sorted(p.name for p in (root / 'third_party/xboxrecomp/patches').iterdir())
numbers = []
for name in patches:
    m = re.match(r'^(\d{4})-[a-z0-9-]+\.patch$', name)
    if m is None:
        print('not a numbered patch file:', name)
        bad += 1
    else:
        numbers.append(int(m.group(1)))
if numbers != list(range(1, len(numbers) + 1)):
    print('patch numbers with a gap or a repeat:', numbers)
    bad += 1
print(f'{len(files)} switch files, {len(envs)} variables, {len(numbers)} patches, {bad} problems')
sys.exit(1 if bad else 0)
