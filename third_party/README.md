# Vendored third-party code

Copied unmodified, with their licences.

| Name | Version | Upstream | Licence | Used for |
|---|---|---|---|---|
| greatest | 1.5.0 | https://github.com/silentbicycle/greatest/archive/refs/tags/v1.5.0.tar.gz (SHA-256 `33e4c93b1b7f631fec173b8675582d1739b1238a5440b7f4ad50582f0cf38c34`) | ISC | test framework (`greatest.h`), tests only |

## Derived code (provenance)

Files in WiiLibs' own modules that are derived from other projects. Each file says so in its header
next to its SPDX line.

| File | Derived from | Licence | What was taken |
|---|---|---|---|
| `wl/xboxrt/x86_decode.c`, `x86_decode.h` | xboxrecomp (https://github.com/sp00nznet/xboxrecomp) `src/platform/mmio_decode.h`, Copyright (c) 2026 sp00nz and contributors | MIT | the decoder of a faulting x86-64 instruction (prefixes, ModRM length, the mov/test/cmp/or/and/movzx cases), rewritten over a portable register file and extended |
| `wl/xboxkrnl/exports.inc` | xboxrecomp `tools/xbe_parser` (`KERNEL_EXPORTS`) and `src/kernel/kernel_bridge.c` (`stdcall_args_for_ordinal`, the data exports) | MIT | the kernel's export names and stdcall argument sizes by ordinal (facts of the retail kernel, as xboxrecomp records them) |
| `wl/xboxkrnl/rtl.c`, `kdata.c`, `hal.c`, `ex.c`, `path.c`, `nt_file.c` | xboxrecomp `src/kernel/kernel_bridge.c`, `kernel_xbox.c`, `kernel_hal.c`, `kernel_path.c` | MIT | in substance: the NTSTATUS to Win32 error table, the data exports' values, the AV option numbers and capability word, the EEPROM defaults, the device layout of the hard disk and its partition table, the DVD media-check answer; the code is written anew over guest accessors |
