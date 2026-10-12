# wl/platform

The platform layer: one header (`platform.h`) between application code and libogc, with a Wii
backend and a dependency-free host backend, so the same code builds for the console and runs in
host tests.

Target `wl_platform` (alias `WiiLibs::platform`), depends on `wl_base`.

| Platform | Backend | Links |
|---|---|---|
| `wii` | `wii/*.c` over libogc | fat, wiikeyboard, wiiuse, bte, aesnd, asnd, ogc, m |
| `host`, `ppc-be` | `host/*.c` stubs over POSIX | pthreads only (no SDL, no GL) |

## Files

| File | What |
|---|---|
| `platform.h` | the platform API: system events and exit, time, memory regions (and fixed-address MEM2 claims, `wl_platform_mem_claim_fixed`) and the data cache, video, GX bootstrap and state shadow, threads, files, raw input, audio output, logging |
| `mem.h`, `mem.c` | budgeted memory system: one heap per tag over MEM1/MEM2, double-buffered frame arena, scene arena stack, leak reports |
| `mem_heap.h`, `mem_heap.c` | the tagged-heap core (first fit, coalescing, 32-byte headers, deterministic) |
| `heap_overflow.h`, `heap_overflow.c` | the overflow heap's portable core: an allocator that is tried first, and a `wl_mem` heap that takes what it cannot hold (ownership by address, realloc between the two, any alignment); see "malloc that spills into MEM2" |
| `heap_sites.h`, `heap_sites.c` | who holds the heap: live blocks counted by the two return addresses that asked for them (the small ones by size), for the wrapped `malloc`'s report; see "malloc that spills into MEM2" |
| `ring.h`, `ring.c` | lock-free single-producer/single-consumer ring (C11 atomics) |
| `present.h`, `present.c` | the display queue: which XFB is drawn, which wait for the GPU or their retrace, which is shown; a portable state machine that `wii/video_wii.c` drives from the retrace interrupt; see "Video" |
| `io.h`, `io.c` | the IO thread: a job ring in, a completion ring out |
| `fs.h`, `fs.c` | asynchronous file reads over the IO thread, read-speed measurement |
| `fs_posix.inc` | the file half of `platform.h` on unbuffered POSIX fds, shared by both backends |
| `posix.h`, `posix.c` | `wl_posix_pread`, `wl_posix_pwrite` (a seek, the transfer and a seek back under one lock, so the descriptor's position is left alone) and `wl_posix_pause`: the POSIX calls devkitPPC's newlib declares and does not define. `wii/posix_wii.c` defines the C library's `pread`, `pwrite` and `pause` over them, as weak symbols (a newlib that gains them wins), so a program calls them by their own names |
| `tev.h`, `tev.c` | TEV programs as data: stages (inputs a b c d, op, bias, scale, clamp, output register, order, swap tables, konst selections), the helpers a translator allocates with (konst slots, register pool, resource merging), a validator, a software evaluator, the fixed GX recipes as programs; see below |
| `backtrace.h`, `backtrace.c` | return-address backtraces for the 32-bit PowerPC frame layout: a portable walker over a read callback, `wl_platform_backtrace` for the running thread, `wl_platform_log_backtrace` for a "where from" log line; see below |
| `profile.h`, `profile.c` | the sampling profiler: a ring of samples (the interrupted pc, link register, thread and the return addresses up its stack), the text dumps, the jitter; `wii/profile_wii.c` is the sampler over libogc's decrementer tick tasks; `tools/wl_profile.py` symbolizes the dumps; see below |
| `profile_arm.h`, `profile_arm.c` | arming the profiler from a control file (`profile.txt` in the program's folder): the settings' parser, the watcher thread that waits for the program's own readiness hooks, samples for a time and writes `profile<N>.txt`; see "Arming it from a file" under the sampling profiler |
| `net.h`, `net.c`, `net_internal.h` | IPv4 TCP and UDP sockets over IOS (libogc) or the host: addresses as host-order values, errors as results, poll for select, the network started in the background; `net.c` has the errno mapping and the address text; see below |
| `gx_common.c` | GX helpers needing no hardware: texture sizes, sampler filter resolution |
| `stack.c` | stack depth measurement: a fill pattern and the high-water mark (`wl_platform_stack_used`) |
| `wii/` | libogc backends (`heap_wrap_wii.c` wraps newlib's malloc family for the overflow heap and includes no libogc header; `tev_wii.c` applies a TEV program to GX): 64-bit atomics (the libatomic calls compilers make; Broadway has no 8-byte atomic instructions), thread-local variables (`__emutls_get_address` for `-femulated-tls`), the crash report (and the backtrace it uses, `backtrace_wii.c`), the exit tracer (`trace_wii.c`), network (`net_wii.c`, libogc's `net_*`), audio (AESND/ASND), exit and system buttons, fs (libfat, IOS 58), gx (with an optional EFB alpha, `wl_gx_set_efb_alpha`), input (WPAD/PAD/keyboard), log sinks, mem, threads (LWP), video |
| `host/` | stubs: backtrace (the real chain on ppc-be, none elsewhere), net (POSIX sockets), audio (test renders blocks), fs (devices map to directories), gx (records draws), input (preloaded state), mem (malloc'd regions), system and log, tev (records the last applied program), threads (pthreads) |

Host test control lives in `host/*_host.h`: `wl_host_audio_render`, `wl_host_fs_set_root`,
`wl_host_gx_state` / `wl_host_gx_record`, `wl_host_input_set`, `wl_host_system_raise`, `wl_host_tev_last`.

## Typical start-up

```c
wl_platform_fs_ios_init(NULL);          // first: an IOS reload drops every IOS handle
wl_platform_system_init();              // reset/power buttons
wl_platform_fs_mount(WL_DEVICE_SD);
wl_platform_log_init("sd:/apps/myapp/logs");
wl_platform_crash_report_install();     // a CPU exception's registers and callers to OSREPORT
wl_mem_init(NULL);                      // or a WlMemConfig with application tags
wl_platform_video_init();
wl_gx_init();
wl_fs_start();                          // IO thread
// per frame: wl_platform_input_poll, wl_mem_frame_begin, wl_gx_begin_frame ... wl_gx_end_frame,
//            wl_platform_log_flush, wl_platform_system_events
```

## TEV programs (`tev.h`)

A `WlTevProgram` describes the GX texture environment as plain data, so a translator (from the
Xbox's register combiners, say) or a hand-written effect can build it, check it and test it on the
host without GX, and apply it on the console with `wl_tev_apply`.

```c
WlTevProgram p;
wl_tev_program_init(&p);                       // swap tables as GX_Init leaves them
int k = wl_tev_konst_slot(&p, 0x80808080u);    // konst colours: 4 slots, equal colours share
WlTevStage *s = wl_tev_add_stage(&p);          // up to 16; a pass-through stage to start from
wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);                // texcoord 0, texmap 0, colour 0
s->kcsel = wl_tev_kcsel(k, WL_TEV_K_RGB);                       // what KONST reads in the colour half
wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO,
                   WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);   // tex * ras
wl_tev_stage_alpha(s, ...);
WlTevReport rep;
if (wl_tev_validate(&p, &rep)) wl_tev_apply(&p);   // apply validates again and refuses a bad program
```

- A stage computes `(d +- ((1-c)*a + c*b) + bias) * scale` for the colour and the alpha apart (or a
  compare, `d + (a > b ? c : 0)`); a stage reads all inputs before it writes its outputs. The inputs
  a, b and c are the low 8 bits of a register; d is the register's full signed 11-bit value, so an
  unclamped result feeds d and goes on. DIVIDE_2 halves the whole sum, bias included, and is valid
  with a bias: `(d +- lerp + bias) / 2`, which is how an exact signed dot product `(lerp - 1/2) / 2`
  takes one stage. That rests on Dolphin's TEV (`((d + bias) << shift + lerp) >> 1`); real hardware
  has not been checked. Every enum equals libogc's value (asserted in
  `wii/tev_wii.c`), so a stage maps to the GX calls field for field.
- `wl_tev_validate` finds what the hardware would reject (stage count, inputs, ops, a bias with a
  compare, registers, texcoord without texmap, reading a texel or the rasterised colour
  with nothing bound, swap tables, konst selections, preloading PREV) and warns about what is
  probably a mistake (a register read that nothing wrote, a value written and never read, a K
  register selected that is neither set nor marked external, no stage writing PREV). A half that
  only passes PREV through (what `wl_tev_add_stage` leaves) is skipped by those warnings.
- `wl_tev_eval` runs a program on one pixel (texels per texmap, the two rasteriser colours) in
  integers, with the hardware's 8.8 blend (`c + (c >> 7)` as the factor), 11-bit unclamped
  registers, swap tables and every konst selection; tests check numeric results with it. It
  matches the hardware to rounding; it has not been compared bit for bit.
- The helpers a translator needs: `wl_tev_konst_slot` / `wl_tev_konst_external`, `wl_tev_pool_*` (a
  register pool, REG0..REG2 before PREV), `wl_tev_needs_*` (the texture, colour channel and swap
  tables a set of inputs wants from a stage, and the colour half's and alpha half's konst
  selection (`wl_tev_needs_konst_color` / `_alpha`); two operations share a stage when their needs
  merge), `wl_tev_channels_used` / `wl_tev_texcoords_used` (counts, for `GX_SetNumChans` and
  `GX_SetNumTexGens`, which `wl_tev_apply` leaves to the caller) and `wl_tev_channels_mask` /
  `wl_tev_texcoords_mask` (the same as sets), `wl_tev_equal` / `wl_tev_hash`.
- `wl_tev_apply_ex` / `wl_tev_apply_unchecked_ex` take `WL_TEV_APPLY_SKIP_SWAPS`, which leaves the
  swap tables alone for a caller that set them once.
- `wl_tev_apply` bypasses the `wl_gx` state shadow: call `wl_gx_invalidate_shadow` after it (and
  after the draws that use the program), as for any direct GX use. `wl_gx_set_recipe` itself builds
  its recipe with `wl_tev_recipe` and `wl_tev_apply_unchecked`.
- Not modelled: indirect texturing (GX_ALPHA_BUMP channels are rejected), S10 register preloads,
  the alpha half's R8/GR16/BGR24 compares.

## Backtraces (`backtrace.h`)

```c
uint32_t callers[8];
uint32_t n = wl_platform_backtrace(callers, 8, 0);   // callers[0]: where this call was made from
wl_platform_log_backtrace("net", "no memory for a packet", 0);   // "[E][net] ... from 80012345 8003a0f0 ..."
```

The addresses are the 32-bit return addresses up the frame chain, for `addr2line -e program.elf`
(or the linker map). Each PowerPC EABI/SysV frame holds its caller's frame at offset 0, and the
return address into a function is kept in the frame above the one that returns to it, so the walk
needs no unwind tables and no allocation, and works from a log path, an exception handler or a
failed `malloc`. On the Wii only MEM1 and MEM2 are read, so a stack overrun that damaged the chain
ends the walk instead of raising a second fault. The walk ends at the thread's first frame, at a
back chain that does not rise, at a misaligned or unreadable frame, and at a return address of 0.
`wl_platform_backtrace_from(sp, lr, ...)` does it for a context given by its registers (the crash
report uses it: the interrupted code's link register comes first). The portable walker
`wl_backtrace_walk` reads through a callback, which is how the tests run it on any host against a
synthetic stack. On the host stub of an x86 machine, whose frames are not PowerPC's, the functions
return 0; on the `ppc-be` test build they walk the real chain.

Optimised code may inline or tail-call, so the addresses name call sites that still exist; the
functions that were inlined away are not listed.

`wl_platform_trace_exits(<executable>)` (in `cmake/WiiLibs.cmake`) makes a program log every way
out of itself: `exit`, `_exit`, `abort` and `SYS_ResetSystem` write `exit(0) from 80012345 ...`
(tag `exit`) and flush the log, then go on. It is a link-time `--wrap` of those four symbols to
`wii/trace_wii.c`; a build without CMake passes `--wrap=exit --wrap=_exit --wrap=abort
--wrap=SYS_ResetSystem` itself.

## Sampling profiler (`profile.h`)

Where does a frame's time go, function by function? A periodic interrupt records where the
program was, into a ring, and `tools/wl_profile.py` turns the samples into tables.

```c
static uint32_t storage[1000 * 20 * 14];            // (4 + depth) words a sample
WlProfileRing ring;
wl_profile_ring_init(&ring, storage, sizeof storage / 4, 10, WL_PROFILE_STOP_WHEN_FULL);
wl_profile_start(&ring, 1000);                      // 1000 samples a second
...                                                 // the frames to profile
wl_profile_stop();
WlProfileDumpInfo info = {.hz = 1000, .note = "a10, 20 s of play"};
wl_profile_dump_file(&ring, &info, "sd:/profile.txt");   // and/or wl_profile_dump_log(&ring, &info)
```

```sh
tools/wl_profile.py program.elf profile.txt                 # or the Dolphin log: the lines can sit among others
tools/wl_profile.py program.elf profile.txt --top 40 --callers update --tree 2 --timeline --hot 20
```

- **The interrupt.** libogc2's decrementer interrupt saves the interrupted thread's registers into
  that thread's `KThread::ctx` and then runs the tick tasks (`tuxedo/tick.h`) with interrupts off,
  on the exception stack. `wii/profile_wii.c` is a periodic tick task: it reads pc, lr and r1 from
  `KThreadGetSelf()->ctx`, and records them with the time base's low word and the thread. Nothing
  is allocated or locked, and no system call is made. The rate is 1 to 10000 a second; the period
  varies by up to a sixteenth either way (`wl_profile_jitter`), so a sampler at 1 kHz does not lock
  to a 30 Hz frame.
- **A sample** is `4 + depth` words (depth 0 to 16): time, thread, pc, lr, then the return addresses
  up the stack (`wl_platform_backtrace_from`, which reads only MEM1 and MEM2, so a damaged stack
  ends the walk). Ten callers make 56 bytes; a second of play at 1 kHz is 56 KB. The ring is the
  caller's storage. `WL_PROFILE_STOP_WHEN_FULL` keeps the first samples and counts the rest
  (`dropped`); `WL_PROFILE_OVERWRITE` keeps the latest.
- **Cost.** Per sample the interrupt runs about 400 instructions for ten callers (the exception
  entry and exit, the tick task list, the stack walk, the 56 bytes written, the next period): about a
  microsecond, so 0.1% of the processor at 1 kHz and 1% at 10 kHz. That is counted from the code,
  not measured on a console; the on-console test measures the same work with and without 10000
  samples a second and logs it (`INFO profile cost`). The ring's lines of the data cache are the
  other cost: a sample writes one or two.
- **The dump** is text: `WLPROF 1 hz=... tbhz=... depth=... samples=... total=... dropped=...
  thread=... note=...` and a `WLPS tick thread pc lr callers...` line per sample (hexadecimal; the
  callers up to the last non-zero), each under 200 characters. `wl_profile_dump_log` writes them with
  `SYS_Report` (Dolphin's OSREPORT log, not the log ring or the SD log file), `wl_profile_dump_file`
  to a file (`fopen`, so `sd:/...` on the console). A sample is about 95 characters at depth 6 and
  130 at depth 10; 10000 samples through the UART is over a megabyte.
- **The tool** reads the ELF's symbol table (pyelftools) and the dump (any file with those lines in
  it; a log with several profiles gives several reports). It prints the sampling summary, each
  thread's share, and the top functions by their own samples and by samples anywhere on the stack;
  `--callers FUNC` gives who calls a function and where its time goes, `--tree [MIN%]` the call tree
  from the outermost recorded frame down, `--hot [N]` the hottest instructions with source lines
  (addr2line), `--timeline [S]` the busiest functions in each step of S seconds, `--thread N` one
  thread (1 is the busiest), `--collapsed FILE` folded stacks for `flamegraph.pl`. A sample's stack
  is its pc, the link register when that lies outside the function pc is in (a function that has
  not called anything yet keeps its caller there; one that has keeps its own return address in the
  stack), then the return addresses; they are looked up one instruction before. Frames the compiler
  inlined are not there, and the walk is `depth` deep, so the tree starts at the outermost frame
  recorded, which for deep code is not the thread's first function.
- **What the samples are.** The sample is taken when the interrupt is taken, so:
  - Code that runs with interrupts off (libogc's critical sections, the IOS calls) is not
    interrupted; its time is credited to the instruction after the interrupts come on again.
  - Under Dolphin the decrementer is delivered at the end of a translated block, so a sample's
    pc is at a block's end (a branch), not at the instruction that was running: a function's
    total is right, the hottest instruction is not. The time base is the emulated one, and
    Dolphin counts about one cycle an instruction, with no cache or memory latency, so its
    profile shows the instructions executed, not where the console waits. On a console, cache
    misses and the GX FIFO and bus waits show up as the time spent on the instruction that waits.
  - A sample says nothing of time spent in other threads' interrupts (audio, IOS) except that the
    interrupted code was late.
- **Arming it from a file (`profile_arm.h`).** A program that an emulator or the Homebrew Channel
  starts cannot be run under a profiler by hand, so it reads `profile.txt` from its folder when it
  starts and, if there is one, launches a watcher thread (`wl_profile_arm_config_load`, then
  `wl_profile_arm_launch`); without the file nothing is allocated or started. The file is lines
  of `name value` (`#` starts a comment, a value is followed by the end of the line, a blank or
  `#`; `name=value` is the same): `seconds 20` (how long to sample), `after 10`, `skip 1`, `repeat 1`
  (profiles one after another), `hz 500` (1 to 10000), `depth 12` (0 to 16), `log 1` (also write
  the samples to the log) and `note words` (the dump header's note). Unknown names and non-numbers
  go to a callback with their line number and leave the settings as they were; the values are then
  clamped. The program supplies three hooks that read its own state from the watcher's thread and
  must not block: `ready` (profile now? it also writes words for the header, such as the level),
  `stop` (end this profile early?), `advanced` (the stretch just profiled is over, so wait for the
  next one: without it a tour would profile one level `repeat` times). `after` and `skip` are the
  hooks' to read from the settings (the watcher does not know their unit). Each run allocates
  its ring (`seconds * hz` and a tenth over, at most `max_ring_bytes`, 4 MiB by default, with a
  warning when it clips), samples until `seconds` have passed by the clock or `stop` says so,
  writes `<folder>/profile<N>.txt` (`wl_profile_arm_dump_path`, N from 1) and the log lines, and
  frees the ring. The log carries `[profile] sampling ...`, `<path>: WL_OK` and `[profile] done`
  (a script can wait for the last in an emulator's output). The decisions are
  `wl_profile_arm_step`, a function of the state, the hooks and a time, which returns what to
  do (nothing, start, stop, finish); `wl_profile_arm_run` carries them out on the calling thread
  against `WlProfileArmOps` (the clock, the sleep, the sampler; each defaults to the platform's),
  so tests run whole tours on a fake clock and a fake sampler.
- **Tests.** `tests/platform/test_platform_profile.c` (host and big-endian: the ring, both modes,
  the jitter, the dump format and file, a sample taken from the real stack on the big-endian
  build), `tests/platform/test_platform_profile_arm.c` (the control file's parser, the step function against a
  fake program, the watcher over a fake clock and sampler, and on a thread; also big-endian),
  `tests/tools/test_wl_profile.py` (parser, symbols, stacks, tables, the command line over
  an ELF made with the host compiler), and `check_profile` in the console runner (a busy loop and
  a second thread under the real decrementer: the samples land in the loop, name its caller and the
  thread, are about 1 ms apart and not evenly, and stop when stopped).

## Sockets (`net.h`)

```c
wl_net_start();                          // after wl_platform_fs_ios_init; returns at once (DHCP takes seconds)
if (wl_net_wait(15000) != WL_OK) { /* no network: WL_ERR_NET_DOWN from every call, or play offline */ }

WlSocket s;
wl_net_socket(WL_NET_UDP, &s);
WlNetAddr me = {wl_net_local_address(), 5000}, peer = {WL_NET_ADDR(192, 168, 0, 7), 5000};
wl_net_bind(s, &me);
wl_net_set_nonblocking(s, true);
wl_net_sendto(s, data, size, &peer);
WlNetAddr from;
int32_t n = wl_net_recvfrom(s, buffer, sizeof buffer, &from);   // n >= 0 bytes, WL_ERR_AGAIN if none yet
```

One header for the console's and the host's network, so the code above runs in a host test over
loopback (`tests/platform/test_platform_net.c`, also on the big-endian build) and on the Wii:

- **Starting.** libogc's `net_init` blocks until DHCP answers; `wl_net_start` calls `net_init_async`
  and `wl_net_state` / `wl_net_wait` report `WL_NET_STARTING`, `WL_NET_UP` or `WL_NET_FAILED`. Every
  socket call returns `WL_ERR_NET_DOWN` until it is up. The network needs IOS's network module, so
  start it after `wl_platform_fs_ios_init` (a reload of IOS drops every IOS handle, sockets too).
- **Addresses.** `WlNetAddr` is the IPv4 address (`WL_NET_ADDR(a, b, c, d)`) and the port in host
  byte order; the lwIP `sockaddr_in` (with its length byte) and the host's are made inside.
  `wl_net_parse_address` takes dotted decimal only.
- **Errors.** libogc's calls return a negative errno in newlib's numbering (its `soc_common.o`
  converts IOS's codes with a table); the host's errno numbers differ for a few. Both become the
  same results: `WL_ERR_AGAIN` for would-block (and a signal), `WL_ERR_NET_INPROGRESS` for a
  non-blocking connect that has started, `WL_ERR_NET_REFUSED`, `_RESET`, `_TIMEDOUT`, `_NOTCONN`,
  `_ADDRINUSE`, `_UNREACH` and so on (`net.h`), `WL_ERR_ARG`, `WL_ERR_NOMEM` (no socket left: IOS has
  about 24), and `WL_ERR_FAILED` for the rest. `wl_net_last_errno` has the raw number for a log
  line. A program that needs another system's numbers (Winsock's `WSAECONNREFUSED` is 10061, not
  10000 + errno) maps from the results.
- **Select.** `wl_net_poll` over up to 64 sockets, on `net_poll` (libogc's `net_select` has a
  16-socket fd_set). `WL_NET_POLL_ERROR` and `_HUP` are reported whether asked for or not; a
  non-blocking connect that finishes shows as writable, and `wl_net_connect_result` says how.
- **What IOS lacks.** `getpeername` (keep the address `wl_net_accept` and `wl_net_connect` gave),
  `FIONREAD` (libogc's `net_ioctl` handles `FIONBIO` only: `wl_net_bytes_available` returns
  `WL_ERR_UNSUPPORTED` on the console) and `SO_TYPE` (`wl_net_socket_proto` answers from a table this layer keeps);
  a socket returned by `wl_net_accept` is blocking.
- **Checked.** The host backend is tested on loopback: UDP with senders, truncation and broadcast,
  TCP connect/accept/stream/shutdown, a non-blocking connect and a refusal, poll with timeouts,
  the errno mapping and the address text. The Wii backend is compiled (with `-Werror`) and the
  console runner checks it when the network comes up (UDP and TCP to the console's own address),
  which needs a Dolphin with a network or a console. `wl_net_connect_result` reads `SO_ERROR`, whose
  numbering through IOS has not been checked against hardware.

## malloc that spills into MEM2 (`heap_overflow.h`)

A program that fills MEM1 (a game linked at a fixed address, say) leaves newlib's heap a few
megabytes, while MEM2 sits behind the budgeted heaps. `heap_overflow` makes `malloc` and its
relatives try newlib's heap first and take what it cannot hold from a `wl_mem` tag in MEM2. Blocks
go back to the heap they came from (by address), and `realloc` moves between the two.

```cmake
wl_platform_heap_overflow(my_game)          # the --wrap flags (Wii only; a no-op elsewhere)
```
```c
// tags[MY_OVERFLOW] = { "overflow", WL_MEM2, 5u * 1024u * 1024u }, then after wl_mem_init:
wl_heap_overflow_enable(MY_OVERFLOW);       // before this call malloc is newlib's alone
...
wl_heap_overflow_log_usage();               // newlib's mallinfo, and the tag's use
```

A build without CMake passes `-Wl,--wrap=<name>` for each name in `WL_HEAP_OVERFLOW_WRAPS`
(`cmake/WiiLibs.cmake`) and `-Wl,-u,__wrap_malloc`.

- **What is wrapped.** `malloc`, `calloc`, `realloc`, `free`, `memalign`, `aligned_alloc`,
  `malloc_usable_size` and newlib's `_malloc_r`, `_calloc_r`, `_realloc_r`, `_free_r`,
  `_memalign_r`, `_malloc_usable_size_r`. devkitPPC's newlib keeps the plain names in objects that
  call the reentrant ones, so what libc allocates itself (`fopen`, `strdup`) reaches the wrappers
  through `_malloc_r`. Its `malloc` calls `_memalign_r` with 64 for sizes above 63 bytes; a spilled
  block is 32-byte aligned (any larger power of two when asked for).
- **Newlib's own calls.** `calloc`, `realloc` and `memalign` call `_malloc_r` inside and cut up
  what it returns as if it were theirs. The primary's calls therefore run with `__malloc_lock` held
  and a flag raised, and a `_malloc_r` that finds it raised goes straight to newlib.
- **Overflow blocks** have a 32-byte header in front of the pointer (size, capacity, a check
  value, the offset to the `wl_mem` block). `free` of a pointer in the overflow heap that is not a
  live block (a double free, an interior pointer) is logged, counted (`bad_frees`) and ignored.
  Each block costs the header plus `wl_mem`'s own 32 bytes.
- **realloc.** `realloc(NULL, n)` is `malloc`; `realloc(p, 0)` frees. A newlib block that cannot
  grow moves to the overflow heap; an overflow block keeps its pointer while the size fits its
  capacity, and otherwise moves to newlib's heap if that has room again, else within the overflow
  heap. When both are full it returns NULL with the block untouched.
- **Failure.** A request both heaps refuse is counted and logged once with the return addresses
  up the stack (for `addr2line` on the ELF); set another report with `wl_overflow_set_fail_hook`.
  Sizes of 0 are never moved to the overflow heap.
- **The usage report** (`wl_heap_overflow_usage`) gives newlib's used and free bytes (`mallinfo`),
  the tag's use, peak and budget, and the free bytes in all: what `GlobalMemoryStatus`-style
  queries should say.
- **Who holds it** (`heap_sites.h`). `wl_heap_overflow_track_sites(storage, bytes, sites,
  min_tracked)` turns on an accounting of the wrapped names' blocks in both heaps: each block of at
  least `min_tracked` bytes is recorded with the two return addresses above the wrapper (its
  caller and the caller's caller, from the EABI back chain), and each site keeps its live bytes and
  blocks, peak and allocations; smaller blocks are only counted, by size class (under 64, 256 and
  1024 bytes). `wl_heap_overflow_log_sites(n)` logs the totals and the `n` sites holding the most,
  for `addr2line`. The tables are the caller's (`wl_heap_sites_storage_bytes`); a block that finds
  them full is counted as untracked. Blocks made before the call are not counted. Each wrapped call
  then costs a `malloc_usable_size` and, for a tracked block, a hash table probe under
  `wl_platform_mem_lock`.
```c
static uint8_t sites[64 * 1024];
wl_heap_overflow_track_sites(sites, sizeof sites, 256, 1024);   // right after the enable
...
wl_heap_overflow_log_sites(12);   // "812 KiB in 12 blocks (peak 900 KiB, 40 made) from 80123456 80234567"
```
- **Tests.** `test_platform_heap_overflow` runs the core against a fake primary allocator,
  `test_platform_heap_wrap` compiles `heap_wrap_wii.c` against a fake newlib that calls its own
  wrapped names the way newlib's objects do and counts any block it was wrongly given (and, last,
  the accounting by site through every name), `test_platform_heap_sites` the accounting's tables
  against a plain model over a long random run; all run on
  the host, under sanitizers and on `ppc-be`. The console runner links with the wrappers and mallocs
  past MEM1 (`ctest -L dolphin`; "WLT SUITE heap_overflow" in its log).

## Hardware facts behind the rules

**CPU.** Broadway is a PowerPC 750CL: single core, 729 MHz, big-endian, 32 KiB L1 I/D, 256 KiB
L2. Never cast byte buffers to structs; read external data with byte-wise loads. The 750 has no
64-bit atomics (`__atomic_*_8` does not link): shared counters are 32-bit. In `ring.c`, devkitPPC
lowers a release store to `lwsync` and an acquire load to load + `isync`; the 750 executes
`lwsync` as a full `sync`, so the ordering holds.

**Memory.** MEM1 is 24 MiB of fast 1T-SRAM (executable, stacks, XFBs, GX FIFO, hot heaps); MEM2
is 64 MiB GDDR3 of which about 52 MiB is usable under libogc (IOS keeps the rest). `mem_wii.c`
claims the top of the MEM1 arena, leaving 4 MiB above the arena low mark for libogc's malloc,
the XFBs (about 1.5 MiB for PAL) and library allocations, and the whole remaining MEM2 arena,
by moving libogc's arena bounds (`SYS_SetArena1Hi` / `SYS_SetArena2Lo`). Default budgets:

| Tag | Region | Default |
|---|---|---|
| `WL_TAG_STACK` | MEM1 | 0.5 MiB |
| `WL_TAG_GXFIFO` | MEM1 | 1 MiB (the FIFO is `WL_GX_FIFO_BYTES`) |
| `WL_TAG_HOT` | MEM1 | 1 MiB |
| `WL_TAG_AUDIO` | MEM1 | 1 MiB |
| `WL_TAG_NET` | MEM1 | 0.5 MiB |
| `WL_TAG_ASSET` | MEM2 | 30 MiB |
| `WL_TAG_AUDIO_BANK` | MEM2 | 6 MiB |
| `WL_TAG_IO` | MEM2 | 2 MiB |
| frame arenas | MEM1 | 2 x 1 MiB |
| scene stack | MEM1 | 3 MiB |

Applications add tags from `WL_TAG_USER` (up to `WL_MEM_MAX_TAGS`), resize built-in ones, or
disable one with budget 0. The host stub sizes its regions like the console (20 MiB, 52 MiB).

**Data cache.** The GPU and DMA read main memory, not the CPU cache: every GPU/DMA buffer is
32-byte aligned and flushed with `wl_gpu_buffer_commit` (`DCFlushRange`) after the CPU writes it;
memory the GPU or DMA wrote is invalidated (`wl_gpu_buffer_invalidate`) before the CPU reads it.
Dolphin hides a missing flush unless its write-back cache emulation is on; hardware does not.

**Video.** `VIDEO_GetPreferredMode` applies SYSCONF: 480p only with progressive scan enabled and
a component cable; PAL consoles get EuRGB60 when enabled, else 576i at 50 Hz (EFB height 528).
Widescreen is a system setting: render at 640x480 and widen the projection. A retrace is a field
in interlaced modes. XFBs are cleared to YUV black (0x10801080), not zero (which is green).

**Presenting without waiting for the display.** The video interface takes a new framebuffer only
at a retrace: libogc writes its registers in the retrace interrupt, after the pre-retrace callback
and before the post-retrace one. So the flip is made there, and the CPU never waits for it:
`wl_platform_video_present` (and `wl_gx_end_frame`) queue the back buffer and return; at each
retrace the pre-retrace callback hands the video interface the oldest queued frame whose GPU work
is done (`wl_gx_end_frame` sends a draw-sync token behind its display copy, and the callback reads
the token register) and which has waited out the interval (`wl_platform_video_set_interval`: 2 is
30 Hz on a 60 Hz display, as a Direct3D presentation interval of two); the post-retrace callback
sees libogc's current framebuffer change and frees the one it replaced. Frames are shown in order,
none skipped. The CPU waits only in `wl_platform_video_back_buffer`, when every XFB is on the
screen or queued: with two XFBs that is a frame finished while the one before still waits for its
retrace (or is held by the interval: the cap on the frame rate, with the thread asleep, not
spinning); a third XFB (`wl_platform_video_set_buffers(3)`, ~600 KB more MEM1) lets the CPU run a
further frame ahead. A frame shown later than the interval asked is counted as late
(`wl_platform_video_dropped_count`, `WlVideoStats.late`).

The draw-done interrupt (`GX_SetDrawDoneCallback`) is not used for the flip: `GX_DrawDone` and
`GX_WaitDrawDone` share one "finished" flag that any draw-done interrupt sets, so a second user of
draw-done (an application calling `GX_DrawDone` itself) could be woken by the frame's interrupt
before its own work was done. Tokens are compared in order (the register holds the last one
passed), so other tokens sent later do not hide the frame's.

**Overlapping frames.** By default `wl_gx_end_frame` still waits for the GPU to finish the frame
(not for the display), so whatever the caller drew from may be changed as soon as it returns.
`wl_gx_set_frame_overlap(true)` drops that wait: the CPU builds the next frame while the GPU
finishes this one, and the next frame's commands queue behind the display copy (end_frame then
waits only for the frame before, so the GPU is at most one frame behind and the double-buffered
frame arena is not overwritten under it). The caller then
keeps memory the GPU reads by address (vertex arrays, textures, TLUTs, display lists) unchanged
until the frame that used it is retired (`wl_gx_frame_index` when it is drawn,
`wl_gx_frame_retired` / `wl_gx_frame_wait` before it is rewritten or freed).

**GX.** Fixed function: up to 16 TEV stages, 8 textures and texture-coordinate generators per
pass, 8 hardware lights, 10 position/normal matrix slots (`wl_gx_load_matrix_slot` uses 1..9).
The front face is clockwise (opposite of OpenGL). Directional lights are point lights far away.
A FIFO overflow or bad state hangs real hardware: the debug validation in `gx_wii.c` catches
bad enums, unaligned buffers and draws with the wrong state. TMEM is about 1 MiB: prefer CMPR
and C4/C8 textures. `wl_gx_copy_efb_texture` is the render-to-texture path; its I4/I8 copies take
the red channel as is (the luminance copy would map black to 16 and white to 235).

**Threads.** libogc LWPs are strictly priority-scheduled on one core: a higher-priority thread
preempts as soon as it is ready, a lower one runs only while higher ones block. So spin-wait
loops starve lower threads (block on a semaphore), and the audio thread never takes a mutex.
Priorities: IO 32, MAIN 64, NET 80, AUDIO 100. devkitPPC's `sched_yield` is a stub that returns
ENOSYS without yielding, so portable code that spins on it starves its own priority's threads:
`thread_wii.c` defines `__wrap_sched_yield` (`LWP_YieldThread`), and `wl_platform` links its users
with `-Wl,--wrap=sched_yield` (a program built without CMake passes the flag itself). libogc's
main thread has a 128 KiB stack with its audio DSP structures just below it, so a deep call chain
corrupts them before anything faults: `wl_platform_run_on_stack` runs a program's main on a thread
with a stack of the caller's size, filled so `wl_platform_stack_used` reports how deep it went.

**Files.** The Homebrew Channel starts apps under IOS 58 (USB 2.0) and keeps AHB access when
`meta.xml` asks; reloading IOS restores AHB protection, so `wl_platform_fs_ios_init` reloads only
when needed and must run before WPAD, sockets and mounts. SD is `fatMountSimple("sd",
&__io_wiisd)`, USB `fatMountSimple("usb", &__io_usbstorage)`; libfat is initialised nowhere else.
Files use unbuffered fds because stdio's 1 KiB buffer would split large reads into many SD
transfers; read in multiples of 512 bytes. FAT cannot rename over a file (the target is removed
first). Expect 4-9 MiB/s from SD cards and 15-25 MiB/s from USB drives; Dolphin's virtual SD says
nothing about hardware speed.

**Input.** The raw button words equal libogc's `WPAD_*`/`PAD_*` values (asserted at build time).
IR is the bounded position when libogc has a fix, else the unbounded one, so the pointer keeps
tracking off-screen. The accelerometer is `WPAD_GForce` x 100. The remote battery byte is 0..200
on the wire, reported as 0..100. Data format and IR resolution are re-applied on every new
connection. GameCube ports are absent on the Wii Family Edition, Wii mini and vWii.

**Audio output.** One stereo PCM16 voice (e.g. 32 kHz) that the DSP resamples to 48 kHz.
AESND copies a stream buffer in 1152-byte steps (2 ms at its 144 kHz maximum rate); a buffer
whose length is not a multiple of that is padded with silence, so the AESND hardware block is
288 stereo frames (1152 bytes, 9 ms) built from fill calls of at most `block_frames`, in a ring of
3. The stream voice must be looped, or the engine stops it at the end of the first buffer. ASND,
the fallback, calls its voice callback once per 1024 output samples and takes one queued buffer
per call, so its block is 1024 frames in a ring of 4. In AUTO mode AESND is tried first and ASND
takes over when AESND consumes no blocks within 600 ms. The DMA callback only moves pointers
(no mixing, locks or allocation in interrupt context); a high-priority thread fills free blocks.

**Logging.** Plain `printf` does not reach Dolphin's log: `SYS_Report` (EXI UART) does, and shows
up as OSREPORT. The SD log file is buffered (4 KiB) and flushed per frame, when full and at exit.

## Dropped from the original code

The original platform layer also carried two game-specific TEV programs (a cel-shading recipe
and a multi-texture character material) with their extra setters; they are not part of this
library. Build such programs on GX directly and call `wl_gx_invalidate_shadow` afterwards.
