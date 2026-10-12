# Vendored third-party code

Copied unmodified, with their licences.

| Name | Version | Upstream | Licence | Used for |
|---|---|---|---|---|
| greatest | 1.5.0 | https://github.com/silentbicycle/greatest/archive/refs/tags/v1.5.0.tar.gz (SHA-256 `33e4c93b1b7f631fec173b8675582d1739b1238a5440b7f4ad50582f0cf38c34`) | ISC | test framework (`greatest.h`), tests only |
| xboxrecomp | v0.13.1, commit `193e2995ffaec871bb3be43a43bd66aa5fccfeb1` (per-file blob ids in `xboxrecomp/UPSTREAM.json`) | https://github.com/sp00nznet/xboxrecomp | MIT | the Python pipeline that lifts an Xbox executable to C (`tools/xbox_recomp.py`, `wl_add_xbox_title`); a subset, see [xboxrecomp/README.md](xboxrecomp/README.md). WiiLibs' changes are patch files in `xboxrecomp/patches/`, applied in the build tree |

## Files derived from third-party code

WiiLibs files that are not copies but derive from a vendored project keep its copyright line next to
their SPDX line.

| WiiLibs file | Derived from | Licence |
|---|---|---|
| `wl/xboxrt/recomp/recomp_types.h` | xboxrecomp `templates/runtime/recomp_types.h` (generated from it, patches applied, by the script below) | MIT, Copyright (c) 2026 sp00nz and contributors |
| `wl/xboxrt/recomp/gen_recomp_types.py` | its edits quote anchors of the same template | MIT, Copyright (c) 2026 sp00nz and contributors |
