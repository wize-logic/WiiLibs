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

## Demand paging (`paging.c`)

The Wii has ~33 MB of frames for 64 MB of guest RAM (MEM2 and what MEM1 spares beside a 12 MB
DOL and the host side's `keep_free_bytes`), and a title touches more than that (Halo's menu ~34 MB,
a level its 22 MB tag cache, 22 MB texture cache and 4 MB sound cache on top). When no frame is free
one is taken back from a page and the page goes to a swap file, to be read back on its next touch.
The same code runs on the host and ppc-be under a frame limit, through `SIGSEGV` and `mprotect`.

- **States.** A physical page is *zero* (no frame, no swap slot: never touched, freed, or given up
  while still zero; a touch gives it a zeroed frame), *resident* (a frame) or *swapped* (a slot in
  the swap file, read back on a touch). A resident page is DIRTY once it changed since its zero fill
  or its read back, and keeps its slot while it is clean, so giving its frame back costs nothing.
- **Victims: a clock over the frames** with the MMU's referenced bits. The Broadway's MMU sets R and
  C in each PTE (a store that hits a TLB entry with C clear searches the table to set it); a clean
  page's PTE is inserted with C clear. The runtime harvests R and C into its own flags whenever a
  PTE goes (a full PTE group, an unmap) and when the clock passes; clearing R also drops the TLB
  entry so the next use sets it again. Each page remembers the PTE group of its two PTEs, so the
  clock finds them without hashing. The init checks that the MMU records them (a page read, then
  written, with both clear); one that does not (an emulator's) gets fallbacks, logged: without R a
  page's use is its fault (the clock drops its PTEs), without C every page mapped counts as changed. At each frame: a pinned page or one in I/O is passed; a page
  used since the last round gets its second chance; a clean page gives its frame back at once; a
  dirty one is left for a write. On the host the referenced bit is a fault (clearing it closes the
  page's EAs, the next touch opens them) and the changed bit too (a clean page is opened read-only,
  its first write faults and marks it).
- **Writes are batched; clean victims first.** The SD card is slow at small writes: the slow path
  gathers the unused dirty pages the clock passes, and only when a whole look finds no clean page
  writes up to 32 of them (128 KB) in one write to consecutive free slots, then takes their frames,
  which leaves free frames for the next touches. A page read back is read alone (4 KB): only what
  is touched.
- **The swap file**: `<the title's hard disk folder>/pagefile.sys` (`WlXbTitleConfig.swap_path`, ""
  for none), the guest RAM's size by default, grown once (the card allocates its clusters then, not
  during play) and kept; its contents mean nothing across runs. Slots are pages; a bit map says
  which are taken, and a dirty page gets a new slot each time it is written (its old one freed).
  Without a swap file only zero and clean pages give frames back, and a touch that needs a frame
  when none can be had is a guest crash ("no frame left"), as before paging.
- **Fault paths (the Wii).** The DSI handler's exception context keeps the fast cases: a first
  touch with a free frame, a first touch that takes a clean page's frame (a look of at most 256
  frames, no I/O), a resident page whose PTE was evicted from the table. A touch that needs I/O (a
  page to read back, or no clean page at hand so a batch must be written) returns to the
  trampoline the device faults use, and `wl_xb_dsi_slow` runs the paging core in the faulting
  thread's own context, which holds the guest lock: it waits for the card there and the access
  runs again. Its table changes run with interrupts off a step at a time, so the fast path on
  another thread (a PTE reload of a pinned page) sees them whole; pages being written or read are
  marked and neither mapped nor taken meanwhile. A slow-path touch from a thread without the guest
  lock, or with interrupts off, is a crash report that says so.
- **Re-entrancy.** The NV2A and APU models and the kernel read guest memory through host pointers
  under the guest lock; any of their accesses may fault and swap, in the middle of a device's
  emulation (itself inside the trampoline: the trampoline nests). That is safe because the paging
  code never touches guest EAs (it reaches frames through the cached BAT view on the Wii, a second
  mapping of the RAM object on the host), its file I/O goes through its own buffer, and the
  kernel's own file I/O only touches guest memory outside the card's driver (its bounce buffer).
  A pointer the host holds stays valid when its page goes: the next access faults it back in. The
  only thing a fault must not take is the other page of an access across two, which it keeps.
- **Pinned pages** (`wl_xb_mem_pin`): never evicted; at most half the frames. The runtime pins
  KeTickCount's page (the service thread writes it without the guest lock). Nothing else needs it
  now: the fault path's own state (host stacks, the DSI frames, the tables) is host memory, not
  guest memory; guest stacks and TLS are touched only by their thread, holding the lock; no DMA
  reads guest memory (file I/O goes through bounce buffers, GX gets textures and vertices from
  host-side copies, the audio output from a host ring). Pin anything a future backend lets a DMA
  or an unlocked thread read.
- **Statistics.** `WlXbMemStats` (evictions, the clean ones and those made in the fault, swap-ins,
  swap-outs and their write calls, bytes, time in swap I/O, slots), a second `[xbirq] status:` line
  every 10 s when paging, and a `thrashing:` warning when more than 128 pages a second (512 KB/s)
  are read back (`wl_xb_mem_watch`, once a second on the service thread).
- **The host at the Wii's size.** `WlXbMemConfig.frame_limit_bytes` (`WlXbTitleConfig.frame_pool_bytes`,
  or `WL_XB_FRAME_MB` / `WL_XB_FRAME_KB` in the environment) pages a host run as the Wii would:
  `WL_XB_FRAME_KB=33792 build/halo-host/halo <title folder> <hard disk folder>`.

## Devices and faults (`mmio.h`, `ppc_decode.h`, `x86_decode.h`)

`wl_xb_mmio_register` claims a range of guest VAs above `0xFC000000` with read and write callbacks
(sizes 1, 2, 4; 8 splits in two). A guest access there faults; the fault handler decodes the
faulting host instruction, calls the device, writes the result into the interrupted registers and
resumes after the instruction:

- the Wii: the DSI handler's slow path, which leaves exception context through a trampoline
  (`dsi_wii.c`) and runs `wl_xb_dsi_slow` in the faulting code's own context, FP state saved;
  PowerPC loads and stores of D and X forms, byte-reversed ones, update forms (`ppc_decode.h`);
- the host: `SIGSEGV` (`fault_posix.c`), with the x86-64 decoder (`x86_decode.h`, derived from
  xboxrecomp's `mmio_decode.h`) on x86-64 and the PowerPC one on ppc-be. The handler notes the
  faulting host PC first (`wl_xb_mmio_fault_pc`), so a device's trace can name the translated
  function that made the access.

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

The hook the generator should emit at loop back edges: `g_xb_attention` is non-zero while a safe
point has work, and `if (g_xb_attention) wl_xb_safe_point();` before a backward jump costs a load
and a branch. A loop that polls plain guest memory has no device access or kernel call to stop at,
and D3D has several that wait for what an interrupt does: on Halo, the flip throttle at
`0x00185694` and the wait at `0x00189325` hold the guest lock forever without it, and with the hook
before each of the title's 6298 backward jumps (tried by hand on a local build) it runs on into its
frame loop.

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
a kernel call, an `rdtsc` or a device access holds interrupts off (see `g_xb_attention` above); the
service thread logs which thread it is once it has waited a second for the lock, and on the host
and ppc-be that thread logs its host PC and the translated functions it is in.

A device with periodic work (the audio processor's frames) adds a tick
(`wl_xb_service_add_tick(fn, user, period_us)`, up to four): the service thread marks it due at its
period and raises `WL_XB_SVC_DEVICE`, and the work runs `fn` with the lock held before the kernel's
handler of the same round, so an interrupt the device raised is delivered at once. Like any work it
runs at the holder's next safe point: a thread spinning on guest memory with the generated code's
back-edge hook gives the device its frames (DirectSound waiting for its GP command word does).

## The GPU and the MCPX (`devices.h`)

`wl_xb_devices_register_mcpx` registers idle MCPX devices: the APU (its sample counter moving at
48 kHz), AC'97 (codec not ready, so `DirectSoundCreate` fails cleanly), the USB host controllers
(two powered ports, nothing connected), the NIC and the flash.

**Audio** (`audio.c`, built when `wl/xboxapu` is): `wl_xb_audio_attach` puts wl/xboxapu's APU and
AC'97 models over the idle ones (the boot code does, unless `WlXbTitleConfig.audio_off`), with
their interrupt lines (5 and 6, level sources) and a service tick (`wl_xb_service_add_tick`, every
4 ms) that runs the APU's frames, moves the AC'97's bus masters as far, and queues the stereo
frames for the output. The title's own DirectSound drives the models (`wl/xboxapu/README.md`):
nothing in a title's code is replaced, nothing is title-specific.

- The Wii: the platform's audio output at 48 kHz (`wl_platform_audio_start`; the AESND voice
  takes the rate as it is, no resampling), fed from a ring of 85 ms. The tick keeps 32 ms queued
  ahead of the output, so the guest's audio clock follows the output's; a guest that falls far
  behind skips time rather than catching up.
- The host (no sound device): frames follow the wall clock. `WL_XB_AUDIO_WAV=<file>` writes
  everything played to a 48 kHz stereo WAV (a valid file each second, whenever the run ends);
  `WL_XB_AUDIO_TRACE=<file>` logs every APU and AC'97 access with its value and the translated
  function that made it (`wl_xb_mmio_fault_pc` and `wl_xb_symbolize`); `WL_XB_AUDIO=0` keeps the
  idle devices (DirectSoundCreate fails, the title runs silent).
- Every 10 s an `xbaudio` status line: frames run (and silent), voices a frame, what a frame costs
  in nanoseconds, notifiers, traps, GP commands, starved stream frames, clipped samples, output
  underruns, AC'97 buffers. The GPU is behind `WlXbGpuOps`
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
Wii's frame pools: the two largest blocks malloc gives, less `keep_free_bytes`; the host's frame
limit, if any; the swap file `<hard disk folder>/pagefile.sys`); threads; the
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
- `tests/xboxrt/test_xboxrt_paging.c`: demand paging under a frame limit of 64 frames (host, ppc-be):
  zero pages, swap-outs in batches and swap-ins, clean pages going for nothing, both EAs of a page,
  pinned pages, relocations and frees of swapped pages, accesses across pages and copies between
  buffers larger than the pool; built again without a swap file (`test_xboxrt_paging_no_swap`).
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
  touches, PTE evictions, a relocation and device accesses through the DSI trampoline on the
  console's MMU, then demand paging: 12 MB of pages over 8 MB of frames, zero pages given back in
  the DSI's exception context and, with a card for `sd:/wl_xboxrt_pagefile.sys`, changed pages
  written out in batches and read back through the trampoline (with FP state live across them).
  Dolphin needs its full MMU emulation to run it:

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
