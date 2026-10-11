# wl/wiidisc — Wii disc extraction via dolphin-tool

Wii discs are encrypted; this module never decrypts anything itself and holds no keys. It finds
Dolphin's command-line tool, runs `dolphin-tool extract` on the DATA partition into a cache,
verifies that files really appeared, and reports counts plus the SHA-1 of `sys/main.dol`
(optionally compared with a reference hash the caller supplies). Target `wl_wiidisc` /
`WiiLibs::wiidisc`. **Host only** (needs `wl_host`: processes, files, SHA-1, JSON).

## API (`wii.h`)

- `wl_wii_find_tool`: lookup order — env `WL_DOLPHIN_TOOL`; `dolphin-tool` on `PATH`; the flatpak
  `org.DolphinEmu.dolphin-emu` (run with `--filesystem=<cache root>`, because the sandbox cannot
  see other paths and silently writes nothing); under WSL the Windows `DolphinTool.exe` (env
  `WL_DOLPHIN_WIN`, default `/mnt/c/Program Files/Dolphin/DolphinTool.exe`; paths are converted
  with `wslpath -w`). `WL_ERR_NO_TOOL` when nothing is found.
- `wl_wii_probe`: reads the header; tells whether the image is a Wii disc (magic `0x5D1C9EA3`
  at 0x18) and returns the 6-character game ID (non-printable bytes replaced by `?`).
- `wl_wii_open(d, image, cache_root, tool, expected_dol_sha1)`: extracts unless cached and fills
  `WlWiiDisc` (root dir with `sys/` and `files/`, image SHA-1, game ID, `from_cache`, main.dol
  SHA-1, `dol_check` = no reference / match / mismatch, file and directory counts, bytes).
  Errors: `WL_ERR_FORMAT` (not a Wii disc), `WL_ERR_NO_TOOL`, `WL_ERR_TOOL_FAILED`,
  `WL_ERR_EMPTY` (tool succeeded but wrote nothing).
- `wl_wii_reference_hash`: optional helper to look a reference hash up in a caller-owned
  known-dump JSON table (`{"dumps":[{"game_code":..,"inner":[{"path":..,"sha1":..,"verified":..}]}]}`).

## Cache

`<root>/<first 16 hex digits of the image SHA-1>/`; root is `cache_root`, else env
`WL_WII_CACHE`, else `~/.cache/wiilibs/wii`. The image hash is memoised by (path, size, mtime)
under `<root>/.stat/`. Extraction goes to `<dir>.part` and is renamed into place, so a
half-written directory is never used.

## Limits

Only the DATA partition; relies on an installed Dolphin. All processes start from argv arrays
(no shell).
