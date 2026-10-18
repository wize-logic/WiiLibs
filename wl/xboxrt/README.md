# wl/xboxrt: the runtime a recompiled Xbox title runs on

Target `wl_xboxrt` (alias `WiiLibs::xboxrt`) depends on `wl_platform` and `wl_base`. A title
lifted from x86 to C (xboxrecomp's pipeline; the generated code's header is
`wl/xboxrt/recomp/recomp_types.h`) runs on it: guest memory at the Xbox's addresses, devices behind
faults, the register file the generated code reads, guest threads under one guest lock,
interrupts, the kernel thunk dispatcher, crash reports, the USB controllers with the gamepads, a
play harness that drives them from a script, and a bench that replays GPU captures through the
runtime's own GPU. The kernel itself is `wl/xboxkrnl`. The boot code
that joins them is `wl_xboxrt_boot` (`boot.h`), and `wl_xboxrt_main` is a `main` over it.
Headers: `mem.h`, `mmio.h`, `regs.h`, `thread.h`, `irq.h`, `thunk.h`, `crash.h`, `xbe.h`,
`devices.h`, `usb.h`, `pad.h`, `input.h`, `play.h`, `bench.h`, `boot.h`, and the decoders `ppc_decode.h` and
`x86_decode.h`.

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

The Wii has about 20 MB of frames for 64 MB of guest RAM (MEM2 and what MEM1 spares beside a 14 MB
DOL, less the host side's `keep_free_bytes` and the GPU's texture arena; the boot logs the pools,
`guest frames: N KB at ...`), and a title touches more than
that (Halo's menu ~34 MB, a level its 22 MB tag cache, 22 MB texture cache and 4 MB sound cache on
top). When no frame is free one is taken back from a page and the page goes to a swap file, to be
read back on its next touch.
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
  which leaves free frames for the next touches. A page read back by a thread's own touch is read
  alone (4 KB): only what is touched. One read back with the guest lock kept (a device's or the
  kernel's own access: the NV2A walks a vertex or texture buffer page after page) takes the file
  copies after it at the following offsets in the same read, up to 8 pages (32 KB) with frames had
  without I/O, clean and not marked used: the card's time is mostly its seek. a10's opening
  cutscene on the host at the Wii's memory: 486 such reads became 130.
- **Nothing written or read that need not be.** A changed page found zero again when its batch is
  gathered becomes a zero page (no write, and its next touch zero-fills it). A file read into guest
  memory (`wl_xb_copy_in`) overwrites whole pages: a swapped one among them forgets its slot first
  (`wl_xb_pg_discard`), so the read does not fetch from the swap file bytes it is about to replace.
  A title that knows a buffer is dead (it writes it again before reading it) says so with
  `wl_xb_va_discard`: its whole pages give back their frames and slots and read zero (Halo: the
  texture cache's pages a movie or a map's copy borrowed, when they come back to the cache).
- **File-backed pages.** The kernel's file reads (NtReadFile) tell the core which whole pages they
  filled from a file's bytes (`wl_xb_file_pages`): such a page is made clean and remembers its file
  and offset in place of a slot, so while unchanged it gives its frame back with nothing written and
  a touch reads it from the file again (the airlock's swap-outs: 12955 -> 1387). The file is read
  through the title's own handle (no second handle on a file the title writes: a FAT driver's two
  handles disagree about its clusters), so before its bytes change or the handle goes the pages
  forget it (`wl_xb_file_forget`: one without a frame is read back first, all are marked changed):
  a write through any handle on the file, a smaller size, a truncating open, a rename (every handle
  reopens) and a close. A program that writes a file the kernel has open behind its back says so
  first (`wl_xk_file_changed`: the play harness's checkpoint snapshots over z:\savegame.bin).
  The XBE's sections are loaded the same way (`wl_xbe_load_backed`: the boot keeps default.xbe
  open), so its x86 code and constants never go to the swap file; a page two sections share, or
  one with a section's zero tail, stays an ordinary copy.
- **The swap file**: `<the title's hard disk folder>/pagefile.sys` (`WlXbTitleConfig.swap_path`, ""
  for none), the guest RAM's size by default, grown once (the card allocates its clusters then, not
  during play) and kept; its contents mean nothing across runs. Slots are pages; a bit map says
  which are taken, and a dirty page gets a new slot each time it is written (its old one freed).
  Without a swap file only zero and clean pages give frames back, and a touch that needs a frame
  when none can be had is a guest crash ("no frame left"), as before paging.
- **The page cache.** A share of the frames (`WlXbMemConfig.page_cache_bytes`: by default a twelfth
  of a pool of 16 MB or more; `WL_XB_PAGE_CACHE_KB` on the host) holds pages packed small
  (`page_pack.c`, an LZ77 of the LZ4 kind written for a 4 KB page): a page that gives its frame
  back clean with a copy (its file, a swap slot) is packed into a ring there too, and its next
  touch unpacks it instead of reading the card (`zcache_hits`). A page that does not pack into
  3 KB is marked and not tried again until it changes; an entry goes with its page's copy. Most
  of a level's paging is pages coming back soon after they went, so a share of the frames packed
  holds more of them than as frames.
- **Fault paths (the Wii).** The DSI handler's exception context keeps the fast cases: a first
  touch with a free frame, a first touch that takes a clean page's frame (a look of at most 256
  frames, no I/O), a resident page whose PTE was evicted from the table. A touch that needs I/O (a
  page to read back, or no clean page at hand so a batch must be written) returns to the
  trampoline the device faults use, and `wl_xb_dsi_slow` runs the paging core in the faulting
  thread's own context, which holds the guest lock: it waits for the card there and the access
  runs again. A read back gives the guest lock up while the card works (`wl_xb_pg_touch`,
  `wl_xb_block_begin`), as an Xbox thread waits in its I/O, unless the touch is inside a kernel
  call or a device's emulation (their C code expects the lock throughout), at DISPATCH_LEVEL, or in
  the deterministic mode. The page is in transit meanwhile: marked IO, its frame its own; a thread
  that meets it (a touch, a free, a move, a pin, a file's forget) waits for that read (with the
  guest lock kept: the reader needs only the card's lock) instead of reading it again or taking its
  frame. Every transfer on the swap file and the files is made under the card's lock. On a real
  Wii, a10's opening cutscene froze 2-3 s with ~1 s a window of paging I/O under the guest lock
  (the reads also waited for the kernel's streamed reads on the card); on the host 571 of its reads
  now run without the lock. Its table changes run with interrupts off a step at a time, so the fast path on
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
- **The write watch** (`wl_xb_phys_watch`, `wl_xb_phys_written`): whether a range of guest memory
  was stored to since a stamp, from the same C bits, so that a cache of something made from guest
  memory (the GPU's converted textures) need not read or hash it again. Each page has a write epoch,
  set from a global count when a store to it is seen (a C bit harvested anywhere, the host's write
  fault) or its bytes change otherwise (freed, moved); arming harvests and clears the pages' C bits
  (with the `tlbie`: the TLB keeps C too) and takes the count; a look harvests and clears them
  again and compares. A watched page's PTE never carries C ahead of a store, and the host opens a
  watched page for writing only on a write. A swap-out and read back keep the page's bytes and its
  epoch. Not where the MMU does not record C, and never for a pinned page on the host (open for
  writing for good): there the caller compares the bytes.
- **Statistics.** `WlXbMemStats` (evictions, the clean ones, those made in the fault and those found
  zero, swap-ins and those a file read spared, swap-outs and their write calls, bytes, time in swap
  I/O, slots), two more `[xbirq] status:` lines every 10 s when paging (`paging` and `swap`: the
  Wii's log cuts a longer one), and a `thrashing:` warning when more than 128 pages a second (512 KB/s)
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
polls a register until an interrupt moves a word in memory (D3D's fence wait) depends on it. A
holder at DISPATCH_LEVEL or above (its KPCR's IRQL) keeps the lock, as it keeps the console's one
processor: the interrupts its IRQL lets in run at its safe points, but no other thread runs and no
DPC until it lowers the IRQL. DirectSound edits its voice lists so, with no other lock. An access
that is not to a device, or that the decoder refuses (`lmw`, `dcbz`, `lwarx`, FP), is a crash
report.

## The register file (`regs.h`, `recomp_symbols.inc`)

The generated code reads and writes globals: `g_eax`..`g_edi`, `g_esp`, `g_ebp`, `g_seh_ebp`,
`g_fs_base`, `g_df`, the x87 stack (`g_fp_stack`, `g_fp_top`, control word `0x037F` at reset,
`g_fp_cmp`, `g_fp_cc`), `g_xmm0`..`7` (`RecompXmm`) and `g_mm0`..`7` (`RecompMmx`), little-endian
unions, and the process globals `g_xbox_mem_offset`, `g_xbox_code_lo`/`hi`, `g_force_return` and the
indirect-call trace (`g_icall_trace`, `g_icall_trace_idx`, `g_icall_count`). `recomp_symbols.inc`
lists them once: `regs.c` defines them, `regs.h` declares them and builds `WlXbCpu` (the per-thread
part, saved and loaded when the guest lock changes hands), and `tests/xboxrt/test_xboxrt_abi.c`
compiles the list against the generated code's header so a type that differs does not compile.
One entry is the runtime's own: `g_eflags_kept`, the EFLAGS bits (AC, ID) a guest sets with
`popfd` and reads back with `pushfd` (`thunk.c`'s `recomp_unimpl`, which also does the 16-bit
`pushf`/`popf` and `cpuid`), kept per guest thread as `g_df` is.

## The functions generated code calls (`thunk.h`)

| Function | Provided by | What |
|---|---|---|
| `wl_xb_lookup(va)` | here | what every indirect call and jump asks (`RECOMP_RESOLVE`): the three below in order, the manual and generated answers kept in a direct-mapped cache of 4096 entries (the generated table is a binary search); kernel thunks are never kept, as their lookup names the slot the dispatcher runs |
| `recomp_lookup(va)` | the generated `recomp_dispatch.c` | VA to translated function. Referenced weakly here: generated code must be linked as objects (sources of the executable, or an `OBJECT` library), since nothing would pull it out of a static archive |
| `recomp_lookup_manual(va)` | weak default here | the title's hand-written functions: the default asks the hook set with `wl_xb_set_manual_lookup` (`WlXbTitleConfig.manual_lookup`) |
| `recomp_lookup_kernel(va)` | here | the kernel thunk slots `0xFE000000 + slot * 4`: the dispatcher pops the return address, makes the call a yield point, runs the export and pops its stdcall bytes |
| `recomp_icall_fail_log`, `recomp_icall_not_code_log` | here | an indirect call to nothing translated (rate-limited log) |
| `recomp_debug_service(service, arg)` | here | `int 2Dh` debug prints (an `ANSI_STRING`) |
| `xbox_ReadTimeStampCounter()` | here | `rdtsc` at 733.33 MHz; a yield point |
| `recomp_setjmp_slot(buf)`, `recomp_guest_longjmp(buf, v)` | here | the guest CRT's `setjmp`/`longjmp` as host jumps, per thread |
| `recomp_unimpl`, `recomp_trace_*`, `recomp_abi_violation_log`, `recomp_call_profile_hit` | weak defaults here | the lifter's optional diagnostics |
| `wl_xb_port_in`/`wl_xb_port_out` | here | x86 port I/O: reads give 0 (the video encoder's field flag at `0x80C0`: progressive), writes are dropped |

The hook at loop back edges: `g_xb_attention` is non-zero while a safe point has work, and the
generated code does `if (g_xb_attention) wl_xb_safe_point();` (`RECOMP_BACKEDGE()`, patch 0008)
before every backward jump, for a load and a branch. A loop that polls plain guest memory has no
device access or kernel call to stop at, and D3D has several that wait for what an interrupt does:
on Halo, the flip throttle at `0x00185694` and the wait at `0x00189325` held the guest lock forever
without it (the title has 6298 backward jumps). In the deterministic mode the flag stays set, so
every backward jump is a safe point that moves the virtual clock.

## Threads and the guest lock (`thread.h`)

The Xbox has one CPU, and titles rely on it. Each guest thread is a host thread (an LWP with a stack of
twice its guest stack, 64-256 KB, on the Wii, a pthread elsewhere: translated frames are C frames), and one guest lock, a baton
passed by hand, decides which runs guest code. The holder gives it up when it waits
(`wl_xb_wait`), ends, or reaches a yield point (a kernel call, an `rdtsc`, a device access) while a
thread of higher priority is ready, or one of equal priority and its 10 ms quantum is over. The
next holder is the highest-priority ready thread, first come first served within a priority.
`wl_xb_thread_setup_guest` gives a thread its guest stack (with a guard page), its KPCR (NtTib with
the SEH chain end, StackBase/StackLimit, Prcb, IRQL, CurrentThread) and KTHREAD, and its TLS block
laid out as xboxrecomp does (the slot XAPI reads at StackBase). `wl_xb_call_guest` calls guest code
from C (stdcall or cdecl). `wl_xb_thread_create_host` runs a C routine as a guest thread (its own
guest stack, KPCR and TLS, under the lock, not one of the title's threads): runtime code that reads
guest memory, calls guest code or waits as a title thread would (the play harness).

## Interrupts (`irq.h`)

Interrupt lines (edge-asserted, or level sources a device answers), the kernel's handler for
timers, ISRs and DPCs, `KeTickCount` kept moving in guest memory, and the vertical blank (the
retrace interrupt on the Wii, a 59.94 Hz timer elsewhere). The work runs with the lock held at the
holder's next yield point, or on the service thread (above every guest priority) when no thread
runs or the holder hands it the lock at a safe point (never from DISPATCH_LEVEL or above, where
the DPCs would run in the middle of the holder's work). It always runs on the service thread's guest
stack: a device access can come in the middle of a translated function whose `esp` the compiler
has not stored back yet, so the interrupted stack is left alone. A guest thread that spins without
a kernel call, an `rdtsc` or a device access holds interrupts off (see `g_xb_attention` above); the
service thread logs which thread it is once it has waited a second for the lock, and on the host
and ppc-be that thread logs its host PC and the translated functions it is in.

Work at each vertical blank beyond the GPU's (the controller ports, the USB frame tick, the play
harness) is added with `wl_xb_service_add_vblank_hook` (up to eight, run in order after the GPU's);
`wl_xb_vblank_count` is the guest's frame clock.

A device with periodic work (the audio processor's frames) adds a tick
(`wl_xb_service_add_tick(fn, user, period_us)`, up to four): the service thread marks it due at its
period and raises `WL_XB_SVC_DEVICE`, and the work runs `fn` with the lock held before the kernel's
handler of the same round, so an interrupt the device raised is delivered at once. Like any work it
runs at the holder's next safe point: a thread spinning on guest memory with the generated code's
back-edge hook gives the device its frames (DirectSound waiting for its GP command word does).

## The deterministic mode (`thread.h`)

`WlXbThreadConfig.deterministic` (`WlXbTitleConfig.deterministic`; on the host
`WL_XB_DETERMINISTIC=1`) makes two runs of a title with the same input run the same guest code in
the same order and see the same times, so a failure seen once is seen again and a run can be
bisected. The guest's clock (`wl_xb_time_us`: `KeTickCount`, `rdtsc`, `KeQueryPerformanceCounter`,
the interrupt and system times, timers and wait timeouts, the vertical blank, the devices' counters,
the audio processor's frames, the play harness's presses) is then virtual:

- it moves on by a fixed cost at each safe point of the lock's holder: 40 ns a backward jump (the
  back-edge hook: `g_xb_attention` stays set, so every backward jump is one), 400 ns a kernel call
  or `rdtsc`, 400 ns a device access (`WL_XB_DET_NS=<jump>,<call>,<device>` on the host);
- when every guest thread waits, it jumps to what is due next (a vertical blank, a timer, a wait's
  timeout, a device's tick);
- the service thread does no work of its own: the vertical blank, ticks and timers come due at the
  holder's safe points, and the service thread is the idle thread, handed the lock only when no guest
  thread is ready;
- a wake, a new thread and a resume put the thread in the ready queue at once, at a point of the
  guest's, instead of when its host thread gets to run; quanta and the starvation boost count the
  virtual clock;
- the system time starts on a fixed date and files report a fixed time.

The title runs as fast as the host lets it (a menu that waits for vertical blanks flies past). On
the host, Halo's a10 runs about 10% slower a frame than without it (two runs of a script give the
same log lines and the same screenshots). Off, nothing changes: `wl_xb_time_us` is the host's clock.

Host load does not reach the schedule: three escape runs at once with a busy loop on each of the
24 cores and a 512 MB direct-I/O loop gave the golden's lines, audio checksums and screenshots. What
the host's clock still drives is only reported: the service thread's 10 s status lines, the stall
and freeze watches. A run that differs under load is first suspected of sharing its hard disk
folder ("Running a title").

## The GPU and the MCPX (`devices.h`)

`wl_xb_devices_register_mcpx` registers the MCPX's devices: the APU (idle, its sample counter
moving at 48 kHz), AC'97 (idle, codec not ready, so `DirectSoundCreate` fails cleanly: what a title
gets with `noaudio`; the audio models below replace both), the USB host
controllers with the gamepads (below), the NIC and the flash (idle).

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
  function that made it (`wl_xb_mmio_fault_pc` and `wl_xb_symbolize`); `WL_XB_AUDIO_HASH=<file>`
  writes running checksums of the APU's 32 mixbins and of the output, a line each second of audio
  (two runs in the deterministic mode, `WL_XB_DETERMINISTIC=1`, compare line by line: a change to
  the APU model that should not change the sound shows it does not); `WL_XB_AUDIO=0` keeps the
  idle devices (DirectSoundCreate fails, the title runs silent; on the Wii a file `noaudio` in the
  title folder does the same).
- Every 10 s an `xbaudio` status line: frames run (and silent), voices a frame, what a frame costs
  in nanoseconds, notifiers, traps, GP commands, starved stream frames, clipped samples, output
  underruns, AC'97 buffers.

**The GPU** is behind `WlXbGpuOps`
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

## The controllers (`usb.h`, `pad.h`, `input.h`)

Retail titles link the XDK's XAPI and drive the USB host controllers themselves: `XInputOpen`,
`XInputGetState`, `XInputSetState` and `XGetDeviceChanges` end in endpoint and transfer descriptors
the title builds in its memory (Halo's `input_update`, `sub_000E3750`, calls all four; its rumble
thread `XInputSetState`). So the gamepads are hardware here, not a replaced API:

- **The host controllers** (`usb_ohci.c`, `usb.h`): two OHCI 1.0a controllers, 0xFED00000 on
  interrupt line 1 and 0xFED08000 on line 9, behind the device faults. The registers keep their
  set/clear and write-one-to-clear semantics; the control, bulk and periodic lists are walked in
  guest memory (physical addresses through the window, little-endian), one done queue per pass and
  never while the driver owns the last, short IN packets as DATAUNDERRUN with the endpoint halted;
  the interrupt is a level source. The lists are walked at each vertical blank (one packet per
  interrupt endpoint: a report a frame) and, for control and bulk transfers, as soon as the title
  fills them or acknowledges a done queue, so enumeration does not wait for frames.
- **The gamepads** (`usb_xid.c`): a Controller S each (045E:0289, interface class 0x58 subclass
  0x42): the standard descriptors, the XID descriptor and capabilities (XAPI's test of "is this a
  controller"), GET_REPORT, the 20-byte report on interrupt endpoint 1, rumble from endpoint 2 or
  SET_REPORT. Xbox port p sits on root hub port 3, 4, 1, 2 as on the console; a pad is plugged in
  once the driver runs with the root hub's change interrupt on (its own scan clears an earlier
  change), one at a time, and unplugged when its port goes.
- **The gamepad as data** (`pad.h`): `WlXboxPad` (XINPUT_GAMEPAD's layout: a digital word, eight
  analogue buttons, two sticks), its report both ways, the rumble report, and the Wii's controllers
  mapped onto it as OpenCE-Wii maps them for Halo (`xinput_wii_map.c`, CC0): the GameCube
  controller; the Classic Controller; the Wii Remote with a Nunchuk, whose IR pointer is the right
  stick (a dead zone in the middle, a squared ramp to full turn at the edge, `wl_pointer_turn`).
  Rumble (two 16-bit motors) is duty-cycled onto an on/off motor.
- **The ports** (`input.h`): each port's state is what the host's controllers press (the Wii's
  Remote, extension and GameCube controller of the same number; none on the host) laid over by what
  the play harness presses; port 1 always has a controller, the others while the host has one,
  unless a script plugs or unplugs them. Rumble goes back to the host controller of the same number.
  `wl_xb_input_frame` runs at each vertical blank (the boot code adds it).

Provenance: `usb_ohci.c` and `usb_xid.c` derive from xboxrecomp's `src/usb` (MIT; the register
semantics, the list walking, the descriptors and the lessons in their comments), restructured over
the device faults and the vertical blank.

## The play harness (`play.h`)

A scripted player, so that runs without a person go through the menus, into levels and through
them, and are checked from their logs: OpenCE-Wii's `port/linux/src/test_input.c` (CC0) rebuilt for
any recompiled title. Started with a setting: `WlXbTitleConfig.play`, on the host `WL_XB_PLAY`
(a host path after `script:`; `WL_XB_PLAY_OUT=<folder>` for screenshots), on the Wii the first line
of `<title folder>/play.cfg`:

| Setting | |
|---|---|
| `script:<file>` | the steps of a file (relative to the title folder, or absolute) |
| `bot:<seed>` | controller 1 plays as the bot (walks, turns, looks, fires every 3 s, jumps every 5, a grenade every 7) |
| `look:<seed>` | controller 1 only turns and looks up and down |

The steps (play.h lists them with their arguments): `wait`, `frames`, `vblanks`; `press`, `hold`,
`down`/`up`/`release`, `stick`, `trigger`; `screen`, `focus`, `select`, `play`, `ticks`,
`checkpoint`, `until <predicate>`, `memory <address>[:size] [&mask] <op> <value>`, `peek`;
`do <action>`, `load <map>`; `bot`, `look`; a title's long steps (`objective 3600`); `log`,
`screenshot`, `shot` (the Wii: the picture the console drew, a BMP in `shots/`), `capture`; `player <n>`, `<n>: <step>`, `connect`, `disconnect`, `bot <n> <seed>`;
`repeat`/`end`; `quit`. Durations are seconds, `<n>f` frames or `<n>t` the title's ticks; a wait
gives up at its timeout (120 s unless given) and stops the script. Every line it logs starts with
`test_input:` (tag `play`): each step as it begins with the script's clock, what the title reports
as it changes (screen, focus, in or out of a game, checkpoints), and the end (`script finished`,
or `script stopped at line N (...): why` with what was shown).

- **Frames** run on a runtime guest thread (`wl_xb_thread_create_host`, priority 31) woken at each
  vertical blank, or, once a title calls `wl_xb_play_frame_point` from its own game loop (Halo does,
  from a function its main loop calls once a frame, through the generated code's trace hook), at
  that point, on the title's thread between two frames. Either way under the guest lock: a title's
  callbacks read guest memory and may call guest code (`wl_xb_call_guest`).
- **What a title registers** (from `wl_xb_title_setup`, a weak hook `wl_xboxrt_main` calls before
  the boot, which a title project's source replaces): text values (`screen`, `focus`), numbers
  (`tick`, -1 outside a game; `tick_rate`; `checkpoints`; `frame`; `menu`; `menu_busy`;
  `player_x_mm`..., logged with `log`), predicates (`play`, any `until <name>`), actions
  (`load`, any `do <name>`), long steps (`wl_xb_play_register_step`: begin, a frame callback that
  fills the controller and says running, done or failed, end; timed out at the line's last number,
  counted in the title's ticks in a game), and a per-frame hook.
- **Timing**: a press holds and releases for at least 100 ms, 4 reports to the title and 2 of its
  `frame`s each; the script's clock counts vertical blanks (59.94 a second, a stall of loading
  counted as a quarter second); in a game, holds and the bot count the game's time played (the
  title's ticks, which a revert to a checkpoint puts back but the time played does not), so a slower
  host moves as far, the bot's input at a given tick is the same on every run, and a bot that dies
  and is put back does not play the same death again and again. A bot step whose game time stands
  still ends after four times its length (and a minute) on the script's clock.
- **Progress**: in a game, a line a game minute says the time played (`played to tick N (the game's
  tick M)`), so a long cutscene or walk that logs nothing else still shows the game moving on;
  `tools/host_run.py` counts these ticks, and ends a run whose game stands still for 5 minutes.
- **Screenshots**: on the host wl/gxsoft draws the next two frames (the first may begin mid-frame)
  and the second becomes `<out>/<name>.png`; on the Wii a one-frame GPU capture (`<name>.cap`, for
  `tools/xbox_nv2a_replay CAP --png-gx DIR`). `capture <frames>` takes a longer capture on both.
  Either step waits for its capture to be written whole before the next step (a `quit` right
  after no longer cuts it short): at most 10 s for a screenshot, 5 s and a second a frame for a
  capture, then the script goes on and logs that it did.
  With paging (the Wii, `WL_XB_FRAME_MB`) the capture's guest memory goes to the file through a
  bounce buffer (`wl_xb_file_write`): the OS cannot write a page that has no frame.
- **Deterministic only in the deterministic mode** (above): otherwise the guest's clocks are the
  host's, so how many game ticks a frame runs depends on the host's speed; only the input is tied
  to the game's ticks.

## The capture bench (`bench.h`)

Exact console numbers for chosen spots of a title, without playing to them: a GPU capture (the play
harness's `capture <frames>` step, `WL_XB_CAPTURE` on the host; wl/xbox's `nv2a_capture.h`, tens of
MB for a few frames of a level) replayed several times through the runtime's own GPU. The play
setting `replay:<file> [runs]` (the first line of `play.cfg` on the Wii, `WL_XB_PLAY` on the host;
the file relative to the title folder, 5 runs unless given) has the boot set up guest memory, the
textures' arena and the GPU as for the title, and then, instead of loading the XBE and starting the
kernel and the entry point, run the bench and leave with its code (the Wii's main then powers off
where no loader started it, which ends a Dolphin run):

```
play.cfg:  replay:play/a10_g1.cap 5
host:      WL_XB_PLAY="replay:a10_g1.cap 5" WL_XB_FRAME_MB=33 ./halo <title folder>
```

Each run replays the capture through a fresh NV2A model (`wl_nv2a_replay_stream`) over the guest
memory the title runs in: the file is read 256 KB at a time (never held whole), its memory records
written into guest-physical memory through the window a 4 KB piece at a time where they differ (so
the MMU's changed bits, the write watch the texture cache checks through, see what the title changed
and nothing more), its register writes driving the model and the GX backend the title draws with
(`wl_xb_gpu_nv2a_gx`), the frames shown. The converted textures are kept from one run to the next, as
from one frame to the next. Lines tagged `xbbench`: each frame's time (the GPU's part: the model and
the backend, without reading the file and putting its memory in; the draws' and the presents'
share), each run's time a frame with its phases (`pusher`: the model's methods and reads; `draws`,
`clears`, `present`: the backend's calls, a present waiting for the console's GPU to finish the
frame; besides: `memory`, `reading`), and a summary (the best run and the mean; the texture cache's
counts; the paging's). The clock is the time base on the Wii. `profile.txt` and `hwcount.txt` in the
title folder arm the profiler and the counters for a bench as for a title.

## The XBE loader (`xbe.h`)

`wl_xbe_parse` reads the image header (entry point and thunk table decoded with the retail or debug
XOR keys, sections, certificate, TLS directory), `wl_xbe_load` copies the headers and sections into
guest memory through a read callback, and `wl_xbe_resolve_thunks` fills the kernel thunk table:
a function export's slot gets `0xFE000000 + slot * 4` (`recomp_lookup_kernel`), a data export's
the address of its variable, and an ordinal with no body a slot whose first call is logged.

## Booting (`boot.h`)

`wl_xb_boot_run(&cfg)` brings it all up and runs the title: on the Wii `wl_mem` (heaps only: the
rest stays malloc's), the SD card, the log and the crash screen; the XBE's header; guest memory (the
Wii's frame pools: the two largest blocks malloc gives, less `keep_free_bytes` and `texture_bytes`;
the host's frame limit, if any; the swap file `<hard disk folder>/pagefile.sys`); the converted
textures' arena (`texture_bytes`, 15 MB, the vertex arrays' cache in its end; `WL_XB_TEXTURE_MB` on the host); threads; the
image; the kernel (`wl/xboxkrnl`); the thunks; the MCPX devices and the GPU (wl/xbox's model when
it is built in, else the stub); the service thread; then the entry point on a guest thread of its
own. It returns the process's exit code. A play setting `replay:...` runs the capture bench instead,
after guest memory and the GPU (above). `wl_xboxrt_main` is a `main`: on the host
`prog <title folder> [hdd folder]` (`WL_XB_TRACE=1` logs every kernel call; `WL_XB_PLAY`,
`WL_XB_PLAY_OUT` start the play harness), on the Wii `sd:/xbox/<WL_XB_TITLE_NAME>`; it calls
`wl_xb_title_setup(&cfg)` first, a weak hook a title's own source may define. The seed file
(`<hdd folder>/recomp_seeds.json`) gets every indirect-call target that had no translated function,
appended to what the file holds. In the same way `<hdd folder>/vsh_programs.inc` gets every NV2A
vertex program the title draws with that is not compiled in (`wl_xb_gpu_set_program_list`): a title
config's `"vertex_programs": "hdd/vsh_programs.inc"` has `wl_add_xbox_title` compile the list to C
(tools/xbox_vsh_codegen.c), and the GPU runs those programs as functions (the generated
`wl_xb_title_vsh_natives`, weak here).

## Running a title

The host: `prog <title folder> [hard disk folder]`; the title folder holds `default.xbe` and is the
guest's D:, the hard disk folder (default `<title folder>/hdd`) its C:, E: and the cache drives. The
Wii: `sd:/xbox/<WL_XB_TITLE_NAME>` (a compile definition of `wl_xboxrt_main`, `title` by default),
the log in `<title folder>/logs`. A title project's own `main` fills a `WlXbTitleConfig`
(`boot.h`); the defaults are `ram_mb` 64, `keep_free_bytes` 12 MB (the host side's memory on the
Wii besides the textures' arena), `texture_bytes` 15 MB, `htab_bytes` 256 KB and a swap file of the
guest RAM's size.

**One run per hard disk folder.** A run writes its hard disk folder (the swap file, `Z/savegame.bin`,
the cache slots), and its paging reads dropped pages back from the files they were read from
(`wl_xb_file_pages`), so another run on the same folder, or a harness putting the folder's files back
for its own run, changes this run's memory under it. In the deterministic mode that is a crash at a
tick that repeats exactly and looks like a race: a Halo script crashed at the same tick and address
every time another process put `savegame.bin` back while it played. On the host
`wl_xboxrt_main` takes an exclusive `flock` on the folder (`wl_xb_hdd_lock`, `boot.h`) for the
process's life and stops with a message when another process holds it; `tools/golden_check.py` waits
for the same lock before its put-back and passes it on to the program (`WL_XB_HDD_LOCKED=1`). A
script that puts files back before it starts the program writes them before the program can refuse,
so it takes the lock too, or each concurrent run has a copy of its own (AGENTS.md, "Disk space").

**Files in the title folder** (read at start; on the host too, except `play.cfg`):

| File | Effect |
|---|---|
| `play.cfg` | the Wii only: its first line is the play setting (`script:`, `bot:`, `look:` or `replay:`, see above) unless the title's config already has one |
| `profile.txt` | arms the sampling profiler (`wl/platform/README.md`, "Arming it from a file"): `after` counts seconds since the entry point started, a profile follows every `seconds` for `repeat` runs, the dumps are `profile<N>.txt` in the title folder (and in the log with `log 1`); symbolize with `tools/wl_profile.py <elf> <dump or log>` |
| `hwcount.txt` | arms the hardware counters (`wl/platform/README.md`, "Hardware counters") |
| `noxf` | GX's transform unit is kept out of the vertices (every vertex through the program on the processor), for an A/B; `WL_XB_NO_XF` on the host |
| `vcache` | the vertex arrays' cache size in KB, its text (1024 when empty); the default is 1 MB, kept in the end of the textures' arena |
| `novcache` | turns the vertex arrays' cache off (beats `vcache`) |
| `noprefetch` | no `dcbt` ahead of the guest's vertex data and push buffer (an A/B of the prefetch; PowerPC builds only have it) |
| `gxnotex`, `gxnotlut`, `gxtlutmem1`, `gxmaxdraws`, `gxnoquad`, `gxnozrestore`, `gxnocopy`, `gxtexgap`, `gxlatez`, `gxvtxinv` | the Wii only: work left out of what GX gets, to find what hangs a console's GPU by halving (`wl/platform/gx_direct.h`, `WlGxBisect`): every texture one fixed 4x4 texture; the C4/C8 ones only (no TLUT loads); the TLUTs loaded from copies in MEM1; a frame's first N primitives only (`gxmaxdraws=N`); no quad of the targets; the back buffer put back without its Z texture; no texture copy out of the EFB; the vertex's texture coordinates in the descriptor's slots of their own names, gaps and all (TEX0, TEX2, TEX3: the layout that hung a console's GPU, now sent in the first slots); the depth test never early (no `GX_SetZCompLoc(GX_TRUE)`); GX's vertex cache invalidated before every primitive. The boot logs the ones on |
| `overlap` | the processor goes on with the next frame while GX draws (by default each frame's end waits for GX to finish it: the caches' frame-late reuse still shows stale content in some frames); `WL_XB_OVERLAP` on the host |
| `noaudio` | the idle APU and AC'97 (DirectSound fails, the title runs silent), for an A/B; `WL_XB_AUDIO=0` on the host |
| `noreadahead` | no host thread reads ahead of a file the title reads from start to end (a map being inflated into a Z: cache slot: 512 KB read while the title inflates the last 128 KB), for an A/B of a level's first load; `WL_XB_NO_READAHEAD` on the host |
| `deterministic` | the virtual clock (`thread.h`'s deterministic mode), so a run of a script on the Wii is the same game as the host's with `WL_XB_DETERMINISTIC=1`: guest RAM (the play step `dump`) and captures compare byte for byte; `WL_XB_DETERMINISTIC` on the host |
| `netrun.txt` | options in the command line's form, one a line (`#` comments), for a run started without arguments (Dolphin): `net=127.0.0.1:7777`, `switch=...`, `play=...`; the command line wins (below) |
| `play/` | where screenshots and captures go (`WL_XB_PLAY_OUT` on the host); scripts and `replay:` captures are looked for relative to the title folder |
| `hdd/pagefile.sys` | the paging's swap file |
| `hdd/recomp_seeds.json`, `hdd/vsh_programs.inc` | what the run found for the next build (below) |

**Runs chosen by the command line, and the network** (`runopts.h`, `wl/platform/net_stream.h`). A Homebrew
Channel loader passes arguments after the DOL, so each run can be chosen from the PC with no change to the SD
card. Each argument is one option:

| Option | Effect |
|---|---|
| `play=<setting>` | the play setting, as the first line of `play.cfg` (`script:`, `bot:`, `look:`, `replay:<capture> <runs>`) |
| `switch=a,b,c=v` | flag files of the title folder, as if they were there: `noxf`, `novcache`, `noaudio`, `deterministic`, `vcache=2048` (the text after `=` is the file's), `profile` and `hwcount` (profile.txt and hwcount.txt with the defaults), and any switch a build reads, such as `overlap` or `noskin` |
| `file=<name>:<text>` | a file of the title folder with this text, `;` for a line end: `file=profile.txt:after 30;seconds 10;repeat 2` |
| `net=<spec>` | the remote log: `<pc ip>:<port>` (the Wii connects) or `listen:<port>` (the PC connects to the Wii) |
| `run=<name>` | the run's name in the remote log's first line |

The files are virtual (`wl_platform_fs_virtual_set`): the code that reads them is unchanged, and nothing is
written to the card. The host's `prog <title folder> [hard disk folder] [option ...]` takes the same options.
With `net=`, every log line goes to the PC, and so do the files the run writes in the title folder and its
`play/` folder when they are closed (`profileN.txt`, screenshots, bench summaries; those over 16 MB are not
sent). The title ends as usual by returning to the loader, which flushes the stream and sends `END`; a PC
that sees a failure line or a silence sends `QUIT`, which the stream's thread acts on whatever the title is
doing; and a stop screen (a crash) leaves by itself after 10 s. All of it is `tools/wii_net_run.py`'s side:

```sh
tools/wii_net_run.py --wii <wii ip> --dol halo.dol --name a30 --out ~/wl-work/netruns \
    --arg 'play=replay:play/a30_peak.cap 5' --arg switch=overlap,profile
```

Once per setup: the Wii's IP (the Homebrew Channel's network screen shows it) and the title folder staged on
the SD card (`sd:/xbox/<title>` with `default.xbe`, the maps and `hdd/`; the options replace only the
small files). With `netrun.txt` and no PC command line, a Dolphin run sends its log too.

**Environment variables** (host only; the Wii has files instead):

| Variable | Effect |
|---|---|
| `WL_XB_TRACE` | any value: log every kernel call |
| `WL_XB_ALLOCS` | `<KB>`: the kernel's allocation census: every contiguous, system or virtual allocation of at least that size logged with its guest callers ("alloc ..."), and `wl_xk_alloc_census_report` (a title's play step) logs each one kept: KB asked, with a frame, not all zero, and a map of where |
| `WL_XB_PLAY` | the play setting: `script:<host path>`, `bot:<seed>`, `look:<seed>` or `replay:<capture> [runs]` |
| `WL_XB_PLAY_OUT` | the folder for screenshots and captures (default `<title folder>/play`) |
| `WL_XB_DETERMINISTIC` | anything but empty or `0`: the deterministic mode |
| `WL_XB_HDD_LOCKED` | `1`: the harness holds the hard disk folder's lock for the run (`wl_xb_hdd_lock` is not taken) |
| `WL_XB_DET_NS` | `<backward jump>,<kernel call>,<device access>` nanoseconds of virtual time (default 40,400,400) |
| `WL_XB_FRAME_MB`, `WL_XB_FRAME_KB` | a frame limit for guest memory, paged as the Wii pages (KB wins) |
| `WL_XB_TEXTURE_MB` | the textures' arena (the Wii's is `texture_bytes`) |
| `WL_XB_HEAP_MB`, `WL_XB_HEAP_KB` | caps the host's heap from the guest memory's creation on, as the Wii's is (`14` imitates it; the software GX and recorders are not counted) |
| `WL_XB_NO_XF`, `WL_XB_VCACHE_KB` | as the files `noxf` and `vcache` (`0` turns the cache off) |
| `WL_XB_CAPTURE` | `path[,frames[,after]]`: a GPU capture of `frames` frames (2) starting `after` vertical blanks in (600) |
| `WL_XB_AUDIO`, `_WAV`, `_TRACE`, `_HASH` | see "The GPU and the MCPX" |
| `WL_XB_PAGE_CENSUS` | `path` (with a frame limit): the page census (`page_census.h`): each guest page's allocation, what used it (the GPU's textures and vertex arrays, the APU, a map file) and the working set each second; `tools/xb_page_census.py path` makes the tables |
| `WL_XB_PAGE_CENSUS_STACKS` | `1` (with the census, x86-64 glibc hosts): who touched each page: every touch's host call stack to `path.stacks` and `path.touch`; `tools/xb_page_who.py path --elf <program> [--alloc VA]` says by toucher (guest functions, the texture cache's paths, the kernel's file reads) |

The imitation of the Wii's memory on the host is `WL_XB_FRAME_MB=24 WL_XB_HEAP_MB=14` (the Wii's before MEM1's rest joined the frame pools; about 33 and 12 since (the arena 15 MB, the host side 12 MB), to be set from a real-Wii log's "guest frames" lines).

**What the log says.** Tag `xbirq`, every 10 s: `status:` lines for the guest clock against the
host's, the guest's pages and frames, the heap's free memory and, with demand paging, the frames
and swap I/O; once a minute the heap's census (who holds it), the threads' host stacks, wl_mem's
heaps (peak/budget) and the scheduler's counts. Tag `xbgpu`, every 600 vertical blanks: draws (and
those into targets, with the stencil, beyond GX or approximated), the depth fits, the textures' cache
(KB kept, converted, evicted, checks and write-watch answers) and the vertex arrays' (draws sent as
indices, KB kept, converted, evicted). Tag `xbaudio`, every 10 s. Two reports name a hang by what
its threads were doing, costing nothing until they fire (a watcher that polled could move a race):
the **stall report** (`stall:`, no scheduling for 20 s: who holds the guest lock, the ready queue's
head and every thread's state, once a minute while it lasts) and the **freeze watch** (`freeze:`,
no new frame for 30 s: each guest thread's return addresses found on its stack and its IRQL, and
on the Wii a 10 s profile of the frozen title into the log, for `tools/wl_profile.py`; once per
freeze). A third names a holder that keeps interrupts waiting: **held off** (`interrupts held off
for N ms`, the service thread kept from the guest lock for a second): the holder's safe points
meanwhile and how many of them kept the lock at a raised IRQL, its return addresses and IRQL
(`held off:`), and where its host thread runs (the host: its PC and translated callers; the Wii: a
0.2 s profile into the log, the first four times). The status line `interrupts held off for at
most N ms` gives the longest such wait of each 10 s from 100 ms up, and `interrupt service:` its
calls and rounds: a backlog of vertical blanks (owed after a hold) runs one a round, eight a call,
and the calls that end with some still owed are counted there; `interrupt lines ... stay asserted`
is only for lines still deliverable after the eighth round. While the service thread waits for the lock,
the holder's safe points mark the devices' ticks due in its place, so the audio processor's frames
go on. A kernel call that waits for the card gives the lock up meanwhile (`wl_xb_block_begin`), as
an Xbox thread waits in its disk I/O: the real Wii's slow SD card held interrupts off for one to two
seconds at every level load, checkpoint revert and profile dump while a read held the lock.
`wl_xb_card_lock` keeps the transfers on one host file handle apart (the kernel's, outside the guest
lock, and the paging's reads of file-backed pages, inside it).

The title's build learns from a run: every indirect-call target with no translated function is
added to the seed file, and every NV2A vertex program drawn that is not compiled in to the program
list (`wl/xbox/README.md`).

## Crash reports (`crash.h`)

A fault that is not a device access, an access the decoder refuses or `KeBugCheck` prints the guest
address and EA, the host PC and the translated function around it (`wl_xb_symbols`, a table a
title may link, or a scan of the dispatch table over the code range), the guest registers, return
addresses found on the guest stack and the last indirect calls, then stops (the stop screen on the
Wii, `abort` elsewhere; `wl_xb_crash_set_stop` replaces it).

## Tests

- `tests/xboxrt/test_xboxrt_runopts.c`: the command line's options and netrun.txt (flag and settings files read
  through the file API and through the profiler's stdio loader, the play and net strings, the refusals, the
  order); `test_platform_net_stream` and `test_wii_net_run` (tools/wii_net_run.py against a pretend Wii)
  cover the stream and the PC side.
- `tests/xboxrt/test_xboxrt_mem.c`: addresses, aliasing, lazy backing, pinned allocations that move
  user pages, reservations, device faults through the host's decoder, the crash path (host, ppc-be).
- `tests/xboxrt/test_xboxrt_paging.c`: demand paging under a frame limit of 64 frames (host, ppc-be):
  zero pages, swap-outs in batches and swap-ins, clean pages going for nothing, changed pages
  cleared to zero going for nothing, file reads over swapped pages not reading them back, both EAs of a page,
  pinned pages, relocations and frees of swapped pages, accesses across pages and copies between
  buffers larger than the pool, file copies read ahead with the lock kept, a read back that gives the
  guest lock up while another thread holds the card (a second guest thread runs meanwhile and waits
  for the page in transit); built again without a swap file (`test_xboxrt_paging_no_swap`).
- `tests/xboxrt/test_xboxrt_decode.c`: the PowerPC decoder over a matrix of forms, the x86-64
  decoder over byte strings (portable).
- `tests/xboxrt/test_xboxrt_pad.c`: the gamepad's report both ways, rumble, merging, the
  GameCube, Classic and Remote mappings (the pointer as the right stick), the rumble's duty cycle,
  and the ports over the host stub's input (portable).
- `tests/xboxrt/test_xboxrt_usb.c`: the host controller and a gamepad driven as XAPI drives them:
  bring-up, the plug-in on root hub port 3, port reset, enumeration (descriptors, address,
  configuration, XID, a stalled string), DATAUNDERRUN, the report on the periodic list, rumble on
  endpoint 2 and SET_REPORT, unplugging and plugging two pads one at a time (host, ppc-be).
- `tests/xboxrt/test_xboxrt_play.c`: the harness against a pretend title (every kind of step,
  frames from the title's frame point, the "test_input:" lines), the parser's errors, the bot.
- `tests/xboxrt/bench_title.c`: a title with no code of its own over `wl_xboxrt_main`, which the
  capture bench's tests run on the capture the NV2A pixel test writes (`xboxrt_capture_bench`;
  `_paged`: 1 MB of guest frames, the textures checked through the write watch) (host).
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
- The physical allocator keeps the low 0x60000 bytes (the retail kernel's image) out of normal
  allocations, and lends them only to a contiguous request nothing else satisfies: Halo pins
  0x61000..0x19A6000, and D3D's handle tables then ask for a page below 16 MB as a10 loads.
- Provenance: `x86_decode.c`/`.h`, `usb_ohci.c` and `usb_xid.c` derive from xboxrecomp (MIT);
  `third_party/README.md` lists them. `pad.c`'s mappings and `play.c` come from OpenCE-Wii (CC0).
