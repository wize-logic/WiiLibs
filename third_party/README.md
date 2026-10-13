# Vendored third-party code

Copied unmodified, with their licences.

| Name | Version | Upstream | Licence | Used for |
|---|---|---|---|---|
| greatest | 1.5.0 | https://github.com/silentbicycle/greatest/archive/refs/tags/v1.5.0.tar.gz (SHA-256 `33e4c93b1b7f631fec173b8675582d1739b1238a5440b7f4ad50582f0cf38c34`) | ISC | test framework (`greatest.h`), tests only |
| xboxrecomp | v0.13.1, commit `193e2995ffaec871bb3be43a43bd66aa5fccfeb1` (per-file blob ids in `xboxrecomp/UPSTREAM.json`) | https://github.com/sp00nznet/xboxrecomp | MIT | the Python pipeline that lifts an Xbox executable to C (`tools/xbox_recomp.py`, `wl_add_xbox_title`); a subset, see [xboxrecomp/README.md](xboxrecomp/README.md). WiiLibs' changes are patch files in `xboxrecomp/patches/`, applied in the build tree |

## Files derived from third-party code

WiiLibs files that are not copies but derive from a vendored project keep its copyright line next to
their SPDX line.

| WiiLibs file | Derived from | Licence | What was taken |
|---|---|---|---|
| `wl/xboxrt/recomp/recomp_types.h` | xboxrecomp `templates/runtime/recomp_types.h` (generated from it, patches applied, by the script below) | MIT, Copyright (c) 2026 sp00nz and contributors | the runtime register model, memory and flag macros, x87/SSE/MMX helpers, with WiiLibs' big-endian and Wii edits |
| `wl/xboxrt/recomp/gen_recomp_types.py` | its edits quote anchors of the same template | MIT, Copyright (c) 2026 sp00nz and contributors | short quoted anchors of the template's text |
| `wl/xboxrt/x86_decode.c`, `x86_decode.h` | xboxrecomp (https://github.com/sp00nznet/xboxrecomp) `src/platform/mmio_decode.h`, Copyright (c) 2026 sp00nz and contributors | MIT | the decoder of a faulting x86-64 instruction (prefixes, ModRM length, the mov/test/cmp/or/and/movzx cases), rewritten over a portable register file and extended |
| `wl/xboxrt/usb_ohci.c`, `usb_xid.c` | xboxrecomp (https://github.com/sp00nznet/xboxrecomp) `src/usb/ohci.c`, `usb_gamepad.c`, `usb_gamepad.h`, Copyright (c) 2026 sp00nz and contributors | MIT | the OHCI 1.0a register semantics (HCR, the root hub ports' set/clear writes with their change bits cleared first, the done head retired on the WDH acknowledgement), the endpoint and transfer descriptor walk (one done queue per pass, none while the driver owns the last, short IN packets as DATAUNDERRUN), the plug-in once the driver listens, the Controller S descriptors, the XID descriptor and capabilities, GET_REPORT and the rumble report; restructured over wl/xboxrt's device faults, the vertical blank's frame tick and little-endian guest accessors |
| `wl/xboxkrnl/exports.inc` | xboxrecomp `tools/xbe_parser` (`KERNEL_EXPORTS`) and `src/kernel/kernel_bridge.c` (`stdcall_args_for_ordinal`, the data exports) | MIT | the kernel's export names and stdcall argument sizes by ordinal (facts of the retail kernel, as xboxrecomp records them) |
| `wl/xboxkrnl/rtl.c`, `kdata.c`, `hal.c`, `ex.c`, `path.c`, `nt_file.c` | xboxrecomp `src/kernel/kernel_bridge.c`, `kernel_xbox.c`, `kernel_hal.c`, `kernel_path.c` | MIT | in substance: the NTSTATUS to Win32 error table, the data exports' values, the AV option numbers and capability word, the EEPROM defaults, the device layout of the hard disk and its partition table, the DVD media-check answer; the code is written anew over guest accessors |
