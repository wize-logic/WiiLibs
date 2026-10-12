# wl/xboxrt: the runtime a recompiled Xbox title runs on

Target `wl_xboxrt` (alias `WiiLibs::xboxrt`) depends on `wl_platform` and `wl_base`. A title
lifted from x86 to C (xboxrecomp's pipeline; the generated code's header is
`wl/xboxrt/recomp/recomp_types.h`) runs on it: guest memory at the Xbox's addresses, devices behind
faults, the register file the generated code reads, guest threads under one guest lock,
interrupts, the kernel thunk dispatcher, crash reports. The kernel itself is `wl/xboxkrnl`. The boot
code that joins them is `wl_xboxrt_boot` (`boot.h`), and `wl_xboxrt_main` is a `main` over it.
Headers: `mem.h`, `mmio.h`, `regs.h`, `thread.h`, `irq.h`, `thunk.h`, `crash.h`, `xbe.h`,
`devices.h`, `boot.h`, and the decoders `ppc_decode.h` and `x86_decode.h`.

Builds on the Wii (the MMU backend), on the host and on ppc-be (a reservation and `SIGSEGV`). Every
guest access is little-endian whatever the host's byte order (`wl_xb_rd32` and the rest; the
generated code's `MEM32` byte-reverses in the access itself).

## Addresses (`mem.h`)

A guest virtual address becomes an effective address with two instructions:
`WL_XB_EA(va) = (va & 0x0FFFFFFF) | ((va >> 3) & 0x10000000)`.

| Guest VA | EA | What |
|---|---|---|
| `0x00000000`..`0x0FFFFFFF` | the same | user space: the XBE image at `0x00010000`, stacks, heaps (`NtAllocateVirtualMemory`) |
| `0x80000000 + P` | `0x10000000 + P` | the contiguous window: physical page `P` (kernel memory, contiguous allocations) |
| `0xF0000000 + P` | the same as `0x80000000 + P` | the write-combined alias D3D uses |
| `0xFC000000` up | `0x1C000000` up | devices (`mmio.h`): never mapped, every access faults |

The host pointer is `g_xbox_mem_offset + EA` (`wl_xb_ptr`): 0 on the Wii, where the MMU maps the
EAs, and the base of a 512 MB `PROT_NONE` reservation elsewhere. Guest RAM is 64 MB of physical
pages (128 for a debug kit); a user page and its window alias are one page of memory.

- **Physical pages**: `wl_xb_phys_alloc(bytes, lowest, highest, align)` hands out contiguous runs
  from the top, as `MmAllocateContiguousMemoryEx` takes them. A pinned range over pages a user
  mapping holds moves those pages elsewhere first (contents and mappings follow): titles ask for
  fixed physical addresses just above the kernel's `0x60000` reserved bytes. The top 64 KB are kept
  for `MmClaimGpuInstanceMemory`.
- **Virtual memory**: `wl_xb_va_reserve`/`commit`/`decommit`/`release`/`alloc`/`query`/`protect`
  with the NT semantics the kernel's `Nt*VirtualMemory` exports need (64 KB reservation granule,
  `MEM_*` states). Protection is recorded, never enforced.
- **Backends**: on the host and ppc-be, a memfd (or an unlinked file) mapped at the window and again
  per user page, released pages punched out. On the Wii, a hashed page table (segment registers 0
  and 1, 64 KB..1 MB, a cache of the runtime's own tables) filled by the DSI handler on first
  touch from frame pools the boot code takes from malloc; frames are zeroed with `dcbz` when first
  mapped; a full PTE group evicts round-robin and the evicted page faults back in.

## Devices and faults (`mmio.h`, `ppc_decode.h`, `x86_decode.h`)

`wl_xb_mmio_register` claims a range of guest VAs above `0xFC000000` with read and write callbacks
(sizes 1, 2, 4; 8 splits in two). A guest access there faults; the fault handler decodes the
faulting host instruction, calls the device, writes the result into the interrupted registers and
resumes after the instruction:

- the Wii: the DSI handler's slow path, which leaves exception context through a trampoline
  (`dsi_wii.c`) and runs `wl_xb_dsi_slow` in the faulting code's own context, FP state saved;
  PowerPC loads and stores of D and X forms, byte-reversed ones, update forms (`ppc_decode.h`);
- the host: `SIGSEGV` (`fault_posix.c`), with the x86-64 decoder (`x86_decode.h`, derived from
  xboxrecomp's `mmio_decode.h`) on x86-64 and the PowerPC one on ppc-be.

**Every device access is a safe point** (`wl_xb_safe_point`, `thread.h`): once the device has
answered, pending interrupt work runs and a thread waiting for the guest lock gets it. A title that
polls a register until an interrupt moves a word in memory (D3D's fence wait) depends on it. An
access that is not to a device, or that the decoder refuses (`lmw`, `dcbz`, `lwarx`, FP), is a
crash report.

## The register file (`regs.h`, `recomp_symbols.inc`)

The generated code reads and writes globals: `g_eax`..`g_edi`, `g_esp`, `g_ebp`, `g_seh_ebp`,
`g_fs_base`, `g_df`, the x87 stack (`g_fp_stack`, `g_fp_top`, control word `0x037F` at reset,
`g_fp_cmp`, `g_fp_cc`), `g_xmm0`..`7` (`RecompXmm`) and `g_mm0`..`7` (`RecompMmx`), little-endian
unions, and the process globals `g_xbox_mem_offset`, `g_xbox_code_lo`/`hi`, `g_force_return` and the
indirect-call trace (`g_icall_trace`, `g_icall_trace_idx`, `g_icall_count`). `recomp_symbols.inc`
lists them once: `regs.c` defines them, `regs.h` declares them and builds `WlXbCpu` (the per-thread
part, saved and loaded when the guest lock changes hands), and `tests/xboxrt/test_xboxrt_abi.c`
compiles the list against the generated code's header so a type that differs does not compile.

## The functions generated code calls (`thunk.h`)

| Function | Provided by | What |
|---|---|---|
| `recomp_lookup(va)` | the generated `recomp_dispatch.c` | VA to translated function. Referenced weakly here: generated code must be linked as objects (sources of the executable, or an `OBJECT` library), since nothing would pull it out of a static archive |
| `recomp_lookup_manual(va)` | weak default here | the title's hand-written functions: the default asks the hook set with `wl_xb_set_manual_lookup` (`WlXbTitleConfig.manual_lookup`) |
| `recomp_lookup_kernel(va)` | here | the kernel thunk slots `0xFE000000 + slot * 4`: the dispatcher pops the return address, makes the call a yield point, runs the export and pops its stdcall bytes |
| `recomp_icall_fail_log`, `recomp_icall_not_code_log` | here | an indirect call to nothing translated (rate-limited log) |
| `recomp_debug_service(service, arg)` | here | `int 2Dh` debug prints (an `ANSI_STRING`) |
| `xbox_ReadTimeStampCounter()` | here | `rdtsc` at 733.33 MHz; a yield point |
| `recomp_setjmp_slot(buf)`, `recomp_guest_longjmp(buf, v)` | here | the guest CRT's `setjmp`/`longjmp` as host jumps, per thread |
| `recomp_unimpl`, `recomp_trace_*`, `recomp_abi_violation_log`, `recomp_call_profile_hit` | weak defaults here | the lifter's optional diagnostics |
| `wl_xb_port_in`/`wl_xb_port_out` | here | x86 port I/O: reads give 0 (the video encoder's field flag at `0x80C0`: progressive), writes are dropped |

Optional hook for the generator: `g_xb_attention` is non-zero while a safe point has work. A loop
that spins on plain guest memory has no device access or kernel call to stop at; generated code may
test the flag at loop back edges: `if (g_xb_attention) wl_xb_safe_point();`.

## Threads and the guest lock (`thread.h`)

The Xbox has one CPU, and titles rely on it. Each guest thread is a host thread (an LWP with a 1 MB
stack on the Wii, a pthread elsewhere: translated frames are C frames), and one guest lock, a baton
passed by hand, decides which runs guest code. The holder gives it up when it waits
(`wl_xb_wait`), ends, or reaches a yield point (a kernel call, an `rdtsc`, a device access) while a
thread of higher priority is ready, or one of equal priority and its 10 ms quantum is over. The
next holder is the highest-priority ready thread, first come first served within a priority.
`wl_xb_thread_setup_guest` gives a thread its guest stack (with a guard page), its KPCR (NtTib with
the SEH chain end, StackBase/StackLimit, Prcb, IRQL, CurrentThread) and KTHREAD, and its TLS block
laid out as xboxrecomp does (the slot XAPI reads at StackBase). `wl_xb_call_guest` calls guest code
from C (stdcall or cdecl).

## Interrupts (`irq.h`)

Interrupt lines (edge-asserted, or level sources a device answers), the kernel's handler for
timers, ISRs and DPCs, `KeTickCount` kept moving in guest memory, and the vertical blank (the
retrace interrupt on the Wii, a 59.94 Hz timer elsewhere). The work runs with the lock held at the
holder's next yield point, or on the service thread (above every guest priority) when no thread
runs or the holder hands it the lock at a safe point. It always runs on the service thread's guest
stack: a device access can come in the middle of a translated function whose `esp` the compiler
has not stored back yet, so the interrupted stack is left alone. A guest thread that spins without
a kernel call, an `rdtsc` or a device access holds interrupts off (see `g_xb_attention` above).

## The GPU and the MCPX (`devices.h`)

`wl_xb_devices_register_mcpx` registers idle MCPX devices: the APU (its sample counter moving at
48 kHz), AC'97 (codec not ready, so `DirectSoundCreate` fails cleanly), the USB host controllers
(two powered ports, nothing connected), the NIC and the flash. The GPU is behind `WlXbGpuOps`
(`mmio_read`, `mmio_write`, `vblank`, `irq_pending`): `wl_xb_gpu_attach` registers the 16 MB at
`0xFD000000`, the vblank hook and interrupt line 3 as a level source. `gpu_stub.c` is an idle NV2A
(PMC boot id, PFIFO idle with GET following PUT, PTIMER, the PCRTC vblank interrupt). When
`wl/xbox/nv2a.h` exists, `nv2a_adapter.c` puts wl/xbox's NV2A model behind the interface instead
(`wl_xb_gpu_attach_nv2a`), with this API:

```c
WlNv2a *wl_nv2a_create(const WlNv2aConfig *cfg); // cfg->phys_ptr(user, phys, bytes), cfg->user
uint32_t wl_nv2a_mmio_read(WlNv2a *gpu, uint32_t offset, unsigned size);  // offset from 0xFD000000
void wl_nv2a_mmio_write(WlNv2a *gpu, uint32_t offset, uint32_t value, unsigned size);
void wl_nv2a_vblank(WlNv2a *gpu);
bool wl_nv2a_irq_pending(const WlNv2a *gpu);
```

`phys_ptr` returns `wl_xb_phys_ptr(phys)`, the window's host address of guest-physical memory. On
the Wii that is an MMU-mapped EA: the CPU may use it, GX and DMA may not.

## The XBE loader (`xbe.h`)

`wl_xbe_parse` reads the image header (entry point and thunk table decoded with the retail or debug
XOR keys, sections, certificate, TLS directory), `wl_xbe_load` copies the headers and sections into
guest memory through a read callback, and `wl_xbe_resolve_thunks` fills the kernel thunk table:
a function export's slot gets `0xFE000000 + slot * 4` (`recomp_lookup_kernel`), a data export's
the address of its variable, and an ordinal with no body a slot whose first call is logged.

## Booting (`boot.h`)

`wl_xb_boot_run(&cfg)` brings it all up and runs the title: on the Wii `wl_mem` (heaps only: the
rest stays malloc's), the SD card, the log and the crash screen; the XBE's header; guest memory (the
Wii's frame pools: the two largest blocks malloc gives, less `keep_free_bytes`); threads; the
image; the kernel (`wl/xboxkrnl`); the thunks; the MCPX devices and the GPU (wl/xbox's model when
it is built in, else the stub); the service thread; then the entry point on a guest thread of its
own. It returns the process's exit code. `wl_xboxrt_main` is a `main`: on the host
`prog <title folder> [hdd folder]` (`WL_XB_TRACE=1` logs every kernel call), on the Wii
`sd:/xbox/<WL_XB_TITLE_NAME>`.

## Crash reports (`crash.h`)

A fault that is not a device access, an access the decoder refuses or `KeBugCheck` prints the guest
address and EA, the host PC and the translated function around it (`wl_xb_symbols`, a table a
title may link, or a scan of the dispatch table over the code range), the guest registers, return
addresses found on the guest stack and the last indirect calls, then stops (the stop screen on the
Wii, `abort` elsewhere; `wl_xb_crash_set_stop` replaces it).

## Tests

- `tests/xboxrt/test_xboxrt_mem.c`: addresses, aliasing, lazy backing, pinned allocations that move
  user pages, reservations, device faults through the host's decoder, the crash path (host, ppc-be).
- `tests/xboxrt/test_xboxrt_decode.c`: the PowerPC decoder over a matrix of forms, the x86-64
  decoder over byte strings (portable).
- `tests/xboxrt/test_xboxrt_abi.c`: the register file and the runtime's functions against the
  generated code's header, when `WL_XBRT_RECOMP_HEADER_DIR` has one.
- `tests/xboxrt/e2e/`: a synthetic title written in C, built for i386, wrapped in an XBE, lifted by
  xboxrecomp's pipeline and run on the runtime and the kernel (needs the header, xboxrecomp's
  tools at `WL_XBOXRECOMP_DIR`, Python and `gcc -m32`).
- `tests/xboxkrnl/test_xboxkrnl.c`: the kernel through its imports; it also covers the thunk ABI,
  threads under the lock, and an ISR and a thread switch delivered to a loop that only polls a
  device register.
- `tests/xboxrt/wii_console.c`, the Wii only: `wl_xboxrt_console.dol` checks the page table, first
  touches, evictions, a relocation and device accesses through the DSI trampoline on the console's
  MMU. Dolphin needs its full MMU emulation to run it:

```sh
python3 tools/dolphin_run.py build/wii/tests/console/wl_xboxrt_console.dol \
    --mmu --until "XBRT DONE" --fail "XBRT FAIL" --seconds 120
```

(`ctest -L dolphin` in the Wii build runs it with the console runner.)

## Notes

- The Wii's file I/O always goes through a host bounce buffer: guest memory is MMU-mapped EAs that
  the IOS's DMA cannot reach.
- A misaligned access that crosses a page maps both pages on the Wii's first-touch path.
- `KeQueryPerformanceFrequency` is 3 375 000 Hz and `rdtsc` 733.33 MHz, the retail console's.
- Provenance: `x86_decode.c`/`.h` derive from xboxrecomp (MIT); `third_party/README.md` lists it.
