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
| `platform.h` | the platform API: system events and exit, time, memory regions (and fixed-address MEM2 claims, `wl_platform_mem_claim_fixed`) and the data cache, video, GX bootstrap and state shadow, threads, files (and the folder of an application, `wl_platform_fs_find_app_dir`), entropy (`wl_platform_random_bytes`), raw input, audio output, logging |
| `mem.h`, `mem.c` | budgeted memory system: one heap per tag over MEM1/MEM2, double-buffered frame arena, scene arena stack, leak reports |
| `mem_heap.h`, `mem_heap.c` | the tagged-heap core (first fit, coalescing, 32-byte headers, deterministic) |
| `heap_overflow.h`, `heap_overflow.c` | the overflow heap's portable core: an allocator that is tried first, and a `wl_mem` heap that takes what it cannot hold (ownership by address, realloc between the two, any alignment); see "malloc that spills into MEM2" |
| `heap_sites.h`, `heap_sites.c` | who holds the heap: live blocks counted by the two return addresses that asked for them (the small ones by size), for the wrapped `malloc`'s report; see "malloc that spills into MEM2" |
| `mem_census.h`, `mem_census.c` | who holds the host's heap: the heap's own figures (`wl_platform_heap_info`: arena, in use, free inside, free above the top, on the Wii counting what is left above sbrk's top and MEM2's arena) and a registry of reporters that subsystems add (`wl_mem_census_add`), asked only when a census is taken (`wl_mem_census_log`); on the host `wl_platform_heap_cap(bytes)` refuses allocations beyond a cap so a run shows what would fail first on the Wii (`wl_platform_heap_exempt` leaves the host's own stand-ins out of the count; glibc only). wl/xboxrt logs it (`WL_XB_HEAP_MB`) |
| `ring.h`, `ring.c` | lock-free single-producer/single-consumer ring (C11 atomics) |
| `present.h`, `present.c` | the display queue: which XFB is drawn, which wait for the GPU or their retrace, which is shown; a portable state machine that `wii/video_wii.c` drives from the retrace interrupt; see "Video" |
| `io.h`, `io.c` | the IO thread: a job ring in, a completion ring out |
| `fs.h`, `fs.c` | asynchronous file reads over the IO thread, read-speed measurement |
| `fs_posix.inc` | the file half of `platform.h` on unbuffered POSIX fds, shared by both backends |
| `file_replace.h`, `file_replace.c` | `wl_file_replace`: replace a file with a copy written beside it (`<file>.wtmp`, synced by the caller), so that a cut leaves the old file or the new one whole; where rename cannot replace a file (FAT) the old one goes aside as `<file>.wold` for the moment between the two renames (`wl_file_replace_aside`, which a host test calls directly). `wl_file_recover(root, depth, fn, user)`: the sweep for the next start, which puts a `.wold` back where its file is missing, drops it where it is not, and drops half-written `.wtmp` copies. POSIX paths, any directory (`sd:/...` under libfat too); tests: `test_platform_file_replace` |
| `dircache.h`, `dircache.c` | `WlDirCache`: directory listings read once, for resolving a name written in another case than the file system has it (`wl_dircache_spelling`: the entry written as it is, else the first that differs in the case only, else as written). Up to 64 folders of 512 entries; a folder not there stays not there, one too large is never kept (the caller looks into it); a file made or removed since the listing was read is answered as the case of a letter only. Thread-safe; the lister is replaceable. Tests: `test_platform_dircache` |
| `posix.h`, `posix.c` | `wl_posix_pread`, `wl_posix_pwrite` (a seek, the transfer and a seek back under one lock, so the descriptor's position is left alone), `WlPlacedReader` (`wl_posix_placed_open`, `_pread`, `_release`, `_close`: a descriptor read at offsets that remembers where it stands, so that a read that starts where the last ended is one `read` with no seek, where `wl_posix_pread` is three, each of which on FAT follows the file's chain of clusters) and `wl_posix_pause`: the POSIX calls devkitPPC's newlib declares and does not define; `wl_posix_sysconf`, sysconf's memory and processor answers for the console; `wl_posix_thread_set_priority` and `wl_posix_thread_set_relative_priority`, a pthread's priority (devkitPPC's `pthread_create` starts every thread at libogc's lowest, 127, and ignores attributes; the definitions are `wii/thread_wii.c`'s and `host/thread_host.c`'s). `wii/posix_wii.c` defines the C library's `pread`, `pwrite`, `pause` and `sysconf` over them, as weak symbols (a newlib that gains them wins), so a program calls them by their own names |
| `timebase.h` | the Wii's time base (60.75 MHz) and its conversions to microseconds and milliseconds with no 64-bit division (`wl_timebase_us`, `wl_timebase_ms`: long division in base 2^16 by a constant, exact for every count; `wl_udiv64_16` for another divisor under 65536), and `wl_timebase_read` (`mftbu`/`mftb` in place, without libogc's call); `wl_platform_time_us` and `wl_platform_time_ms` use them |
| `input_shape.h`, `input_shape.c` | the raw input in the ranges an application wants, with no libogc in it: sticks over their real travel (a GameCube stick's full deflection is 87, the C stick's 74), triggers 0..255 (the Classic Controller's are 5 bits, the GameCube's start at 30), and the Remote's pointer (position across the screen, a short loss held, a play radius against tremor, a dead zone box and a turn ramp) |
| `input_reports.h`, `input_reports.c` | a Wii Remote's reports of one frame folded into one reading, with no libogc in it: every button pressed in any report, the pointer's mean over the reports that had one, the acceleration's peak, the Nunchuk's readings (`wii/input_wii.c` feeds it from `WPAD_ReadPending`); and a GameCube pad's samples taken at every retrace folded into the poll's look (`WlPadTracker`) |
| `input_gesture.h`, `input_gesture.c` | a Nunchuk's forward thrust found in its accelerometer's readings by a change over a short window after a rest, not by a size; no libogc |
| `tev.h`, `tev.c` | TEV programs as data: stages (inputs a b c d, op, bias, scale, clamp, output register, order, swap tables, konst selections), the helpers a translator allocates with (konst slots, register pool, resource merging), a validator, a software evaluator, the fixed GX recipes as programs; see below |
| `backtrace.h`, `backtrace.c` | return-address backtraces for the 32-bit PowerPC frame layout: a portable walker over a read callback, `wl_platform_backtrace` for the running thread, `wl_platform_log_backtrace` for a "where from" log line; see below |
| `profile.h`, `profile.c` | the sampling profiler: a ring of samples (the interrupted pc, link register, thread and the return addresses up its stack), the text dumps, the jitter; `wii/profile_wii.c` is the sampler over libogc's decrementer tick tasks; `tools/wl_profile.py` symbolizes the dumps; see below |
| `overlay.h`, `overlay.c` | drawing for a frame overlay: rectangles, outlines, gauges and text in a built-in 5 by 7 font, as quads through `wl_gx_draw`; see "Frame overlay" |
| `home_menu.h`, `home_menu.c`, `home_menu_draw.c`, `home_internal.h` | the HOME button's menu and the Wii's notices (a Remote with nothing to move with, a weak battery) as a state machine with no console in it, configured by `WlHomeConfig` (texts, an optional reset item, the Classic Controller's buttons, times), and its drawing through the overlay; see "HOME menu and wrist strap reminder" |
| `strap_screen.h`, `strap_screen.c`, `strap_screen_draw.c` | the wrist strap reminder: a plain screen at the start, the controllers ignored for a second, A goes on; configured by `WlStrapConfig`; see the same section |
| `home_shell.h`, `home_shell.c` | both of those joined to the console and the frame overlay: the sound fade, the leave, the overlay hooked only while something shows, the reminder before the menu, flags for a rumble thread |
| `audio_gain.h`, `audio_gain.c` | the audio output's master gain (`wl_platform_audio_fade`): a linear ramp applied to each block after the program's fill function; see "Audio fade" |
| `stop.h`, `stop.c`, `stop_backend.h` | the bring-up text console (libogc's, in one of the video's framebuffers) and the stop screen: a message on it and in the log, then a wait for HOME or START to leave; `wl_platform_missing_symbol` is what `tools/link_stubs.py` points its stubs at; `wii/stop_wii.c` and `host/stop_host.c` are the backends; see "Console and stop screen" |
| `profile_arm.h`, `profile_arm.c` | arming the profiler from a control file (`profile.txt` in the program's folder): the settings' parser, the watcher thread that waits for the program's own readiness hooks, samples for a time and writes `profile<N>.txt`; see "Arming it from a file" under the sampling profiler |
| `hwcount.h`, `hwcount.c`, `hwcount_app.c` | the console measuring itself: the Broadway's performance monitor (PMC1 to PMC4) and GX's counters read once a frame in rotating groups, medians a second, a verdict on what bounds the frame, marks and the FIFO's fill as a timeline; armed by `hwcount.txt`; `wii/hwcount_wii.c` is the Wii's machine; see "Hardware counters" |
| `net.h`, `net.c`, `net_internal.h` | IPv4 TCP and UDP sockets over IOS (libogc) or the host: addresses as host-order values, errors as results, poll for select, the network started in the background; `net.c` has the errno mapping and the address text; see below |
| `gx_common.c` | GX helpers needing no hardware: texture sizes, sampler filter resolution, the ramp texture's image |
| `gx_direct.h` | GX driven directly, for a renderer beyond `wl_gx`'s recipes and its one vertex layout (an emulated GPU's: `wl/xbox`'s NV2A backend): up to two colours and eight texture coordinates a vertex, drawn at once or a vertex at a time into the FIFO (or as the FIFO's own words, `wl_gx_direct_vertex_words`: the position's floats, each colour as 0xRRGGBBAA, the texture coordinates' floats, written as they are on the Wii; a batch of them in one call, `wl_gx_direct_vertices_words`), the transform unit's position matrix and 2x4 texture coordinate generators from a coordinate or the position (`wl_gx_direct_position_matrix`, `wl_gx_direct_texgen`; a layout's `texcoords_generated` for coordinates made from the position, not sent), projective coordinates divided at each pixel (a layout's `texcoords_projected`: up to three (s, t, q) sent after the position as the normal, binormal and tangent, each through a 3x4 generator), any blend factors, its own projection, depth, alpha compare, cull, colour updates, textures on any map; it bypasses and invalidates `wl_gx`'s shadow. The first draw after `wl_gx_direct_begin` or `wl_gx_direct_flush_after` is preceded by eight vertices of zeros in its own format, the remedy for GX's transform unit hanging on a clipped primitive after an EFB copy, a token or a draw-done (libogc's `GX_Begin` sends nothing for `GX_MT_XF_FLUSH`; OpenCE-Wii's finding). Attributes can be indexed (`WlGxDirectLayout.indexed`): the vertex sends a 16-bit index (`wl_gx_direct_vertex_index`, or a draw's in one call, `wl_gx_direct_vertices_index`) and GX reads the position, colour or texture coordinate from an array set with `wl_gx_direct_array` (big-endian floats and colour bytes, 32-byte aligned, written back from the cache), which geometry converted once costs the processor two bytes an attribute a vertex; `wl_gx_direct_arrays_changed` says memory an array came from has changed. A renderer that sets its own vertex descriptors and formats (indexed arrays, matrix indices: a Direct3D device's, OpenCE-Wii's) keeps the flush through `wl_gx_direct_vertex_format` (each direct attribute's size remembered) and `wl_gx_direct_flush_pending` / `wl_gx_direct_begin_format` (the strip sized by libogc's descriptor), and asks `wl_gx_direct_vertex_shape_ok` whether GX reads its vertices right (`gx_vertex.c` the sizes; `wii/gx_flush_wii.c`, libogc's GX alone, so a host harness over a stand-in links it). `wii/gx_direct_wii.c` (its stores into the write-gather pipe are the loops of `gx_pipe_words.h`: the pointer read once, a vertex of a known size without a loop counter, two indices as one 32-bit store; `test_platform_gx_pipe` checks every byte sent against the plain loops, `bench_gx_pipe` counts their instructions under `tools/qemu_icount.py`); `host/gx_direct_host.c` records what was asked (`host/gx_direct_host.h`) |
| `gx_target.h`, `gx_target.c`, `gx_target_internal.h` | render targets on GX's one EFB: who owns it, parking and putting back the back buffer, resolving a target into its memory, clear quads; the portable bookkeeping over a small backend (`gx_target_internal.h`: `wii/gx_target_wii.c`, `host/gx_target_host.c`); see "Render targets" |
| `shutdown.c`, `shutdown_internal.h` | leaving the program: the shutdown hooks (`wl_platform_shutdown_hook`), the wind-down every `wl_platform_exit` starts with (hooks newest first, the audio output and the motors stopped), and the system watcher, the thread that serves the reset and power buttons (`wl_platform_system_watch_start`); see "Leaving the program" |
| `gx_fifo.c` | `wl_gx_fifo_bytes`: the GX command FIFO's size, from the budget of `WL_TAG_GXFIFO` |
| `stack.c` | stack depth measurement: a fill pattern and the high-water mark (`wl_platform_stack_used`) |
| `wii/` | libogc backends (`heap_wrap_wii.c` wraps newlib's malloc family for the overflow heap and includes no libogc header; `tev_wii.c` applies a TEV program to GX): 64-bit atomics (the libatomic calls compilers make; Broadway has no 8-byte atomic instructions), thread-local variables (`__emutls_get_address` for `-femulated-tls`), the crash report (and the backtrace it uses, `backtrace_wii.c`), the exit tracer (`trace_wii.c`), network (`net_wii.c`, libogc's `net_*`), audio (AESND/ASND), exit and system buttons, fs (libfat, IOS 58), gx (with an optional EFB alpha, `wl_gx_set_efb_alpha`; `gx_target_wii.c` is the render targets' copies and quads), input (WPAD/PAD/keyboard), log sinks, mem, entropy (`random_wii.c`: the time base, not cryptographic), threads (LWP), video |
| `host/` | stubs: backtrace (the real chain on ppc-be, none elsewhere), net (POSIX sockets), audio (test renders blocks), fs (devices map to directories), gx (records draws; `gx_target_host.c` records the render targets' operations, `gx_direct_host.c` the direct layer's, each texture map as GX holds it; a renderer set with `wl_host_gx_set_renderer` (`host/gx_renderer_host.h`) is handed every primitive, EFB copy, quad, frame and peek with the state recorded, and on being set the EFB format `wl_gx_set_efb_alpha` last chose, as wl/gxsoft's software GX draws them; the render targets' quads leave the recorded state as the console's leave GX's), input (preloaded state), mem (malloc'd regions), system and log, tev (records the last applied program), entropy (getrandom, /dev/urandom), threads (pthreads) |

Host test control lives in `host/*_host.h`: `wl_host_audio_render`, `wl_host_fs_set_root`,
`wl_host_gx_state` / `wl_host_gx_record`, `wl_host_target_ops`, `wl_host_input_set`, `wl_host_rumble_allowed`, `wl_host_stop_set_hook`, `wl_host_system_raise`, `wl_host_exit_set_hook`, `wl_host_system_reset`, `wl_host_tev_last`.

## Waits with deadlines, condition variables, files at offsets

For code that carries another system's synchronisation and file API onto the console (the Xbox
kernel in `wl/xboxkrnl` is one):

- **Deadlines**: `wl_platform_sem_wait_timeout` (0 tries, `WL_WAIT_FOREVER` blocks). libogc's
  semaphores have no timed wait, so the console tries, sleeps a millisecond and tries again.
- **Mutexes**: `wl_platform_mutex_create_recursive`, `wl_platform_mutex_try_lock`.
- **Condition variables** (`WlPlatformCond`): `wl_platform_cond_create`, `_wait`,
  `_wait_timeout` (false when the deadline passed), `_signal`, `_broadcast`, `_destroy`; LWP
  conditions on the console, pthreads on `CLOCK_MONOTONIC` on the host.
- **Threads**: `wl_platform_thread_detach` (the console joins a detached thread's control block
  once it has finished, at the next create or detach), `wl_platform_thread_exit`,
  `wl_platform_thread_self_id`, `wl_platform_thread_set_priority`. The host pool holds 64 threads.
- **Wall clock**: `wl_platform_wall_time_us`, microseconds since 1970, read once and advanced by
  the monotonic clock (the console's clock is local time).
- **Files**: `wl_platform_fs_write_at` (a gap reads as zeros), `wl_platform_fs_read_some_at` (a
  short read at the end is not an error), `wl_platform_fs_truncate` (both ways),
  `wl_platform_fs_stat` / `_fstat` (`WlPlatformStat`: size, change time, directory, read-only),
  `wl_platform_fs_rmdir`, and directory listing (`wl_platform_fs_dir_open`, `_next`, `_rewind`,
  `_close`; a pool of `WL_FS_MAX_DIRS`). The file pool holds `WL_FS_MAX_FILES` = 32 handles.
- **Retrace hook**: `wl_platform_video_set_retrace_hook`, a function the retrace interrupt calls
  (it may only wake a thread); the host stub never calls it.

All of them are in `tests/platform/suite_threads.c` and `suite_fs.c`, which run on the host,
on ppc-be and in the console runner.

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
wl_platform_system_watch_start(16, NULL, NULL);   // the buttons work from here on, in any stall
// per frame: wl_platform_input_poll, wl_mem_frame_begin, wl_gx_begin_frame ... wl_gx_end_frame,
//            wl_platform_log_flush
// (without the watcher: wl_platform_system_events once a frame, then wl_platform_exit)
```

## Leaving the program

`wl_platform_exit(target)` winds the program down before it leaves, once, whichever thread asks
(a second caller waits for the first): the shutdown hooks registered with
`wl_platform_shutdown_hook(fn, user)` run, newest first, with the target (a place to let a save
finish, or to close files held open for writing); then the audio output is stopped
(`wl_platform_audio_stop`) and every Wii Remote's and GameCube controller's motor
(`wl_platform_input_stop_rumble`); then the log is flushed and closed, the SD card and USB drive are
unmounted (their caches written out) and the program leaves: `WL_EXIT_LOADER` for the Homebrew Channel,
`WL_EXIT_WII_MENU`, `WL_EXIT_POWER_OFF` (`STM_ShutdownToStandby`).

The reset and power buttons (and a Wii Remote's power button) only latch a bit, in an interrupt.
A program that polls `wl_platform_system_events` once a frame hears nothing while it loads.
`wl_platform_system_watch_start(period_ms, tick, user)` starts a thread (16 KiB from `WL_TAG_STACK`,
priority above the main thread's) that collects them every `period_ms`, and leaves: power, the
console's or a Remote's, with `WL_EXIT_POWER_OFF`, reset with `WL_EXIT_LOADER`. Its optional `tick`
runs on the same thread between looks, a place for work that has to go on while the program stalls
(the motors' on and off). While it runs the program must not call `wl_platform_system_events`: the
events go to whoever asks first. The stop screen's own wait for HOME or START stays, and its
motors are stopped.

The hooks run while the program's other threads still run; a hook that must keep another thread
from starting new work (a write to a file) has to stop it itself, and should not wait long.

`wl_platform_system_simulate(events)` makes the watcher see a reset or power press that did not happen:
it logs "reset button (simulated): not leaving" (or "power button ...") at its next look and leaves
nothing. A script that tests how a program reacts to the buttons (its log, its hooks' timing) can press
them without ending the run; real presses are served as ever. `WL_ERR_STATE` while the watcher is not
running. The hooks are not run by a simulated press: `wl_platform_exit` is the test of those.

**Rumble.** `wl_platform_rumble` and `wl_platform_pad_rumble` honour the console's setting for
Wii Remote vibration (SYSCONF's `BT.MOT`, libogc's `CONF_GetPadMotorMode`; `wl_platform_rumble_enabled`):
with it off they only stop the motor. Each call that changes nothing is dropped (every real call
is a report sent to a Remote), and a Remote that reconnects is taken to be still. A motor is
either on or off: for a strength in between, the caller switches it on and off by frame (a duty
cycle), and should keep the switching to some tens a second.

**Sound setting.** `wl_platform_sound_mode` reports the console's mono, stereo or surround setting
(SYSCONF's `IPL.SND`, libogc's `CONF_GetSoundMode`; `WL_SOUND_MONO`, `WL_SOUND_STEREO`,
`WL_SOUND_SURROUND`; stereo when it cannot be read). The output plays what `fill` made, so a
program that follows the setting does so in its mixer: for mono, the mean of the two channels in
both (a few instructions a frame), for surround, as stereo (the console's Dolby Pro Logic is not
encoded).

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
  matches the hardware to rounding; it has not been compared bit for bit. `wl_tev_eval_stages` is
  the same for a renderer (wl/gxsoft): each stage's own texel (GX gives every stage its own lookup,
  so one map read at two coordinates is two texels), no validation (checked once by the caller;
  codes taken within their fields, an unordered texel or colour read as 0).
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


## Hardware counters (`hwcount.h`)

Where the sampling profiler says which functions take the time, this says which hardware resource
the time is spent waiting for: is a frame bound by the processor (and by its memory, its branches or
its instruction count) or by the GPU (and by transform, fill, textures or vertex fetch)?

It is off unless `hwcount.txt` is in the program's folder (`wl_hwcount_arm(folder)`; without the
file nothing is started and every call below tests one flag). Lines of `name value`: `seconds 1` (a
log block every so many seconds), `cpu 1`, `gp 1`, `vcache 1`, `timeline 1` (marks and the FIFO
sampler), `fifo_hz 480`, `selftest 1`, `threshold 0` (MMCR0's THRESHOLD), `first 60` (frames to let
go by) and `raw 0` (also log each group's raw counters, to check a mapping on a console).
`wl_gx_begin_frame` and `wl_gx_end_frame` call the engine, and WiiLibs' own waits for the GPU
(`wait_token`, so `wl_gx_sync`) and for a free display buffer are bracketed, so a program that
uses `wl_gx_*` has nothing to add. A program that waits elsewhere (its own `GX_DrawDone`, a frame
limiter) brackets that wait with `wl_hwcount_wait_begin(WL_HWWAIT_GPU)` or `WL_HWWAIT_DISPLAY` and
`wl_hwcount_wait_end`. Marks: `wl_hwcount_mark(id)` (0 to 15; `wl_hwcount_mark_name` names one).

- **The processor.** The 750CL's performance monitor (IBM, "PowerPC 750CL RISC Microprocessor
  User's Manual", performance monitor chapter; supervisor SPRs MMCR0 952, MMCR1 956, PMC1 953, PMC2
  954, PMC3 957, PMC4 958). MMCR0 holds THRESHOLD (bits 10 to 15), PMC1SELECT (19 to 25) and
  PMC2SELECT (26 to 31), MMCR1 PMC3SELECT (0 to 4) and PMC4SELECT (5 to 9), with IBM's bit 0 the most
  significant; the registers are written frozen (MMCR0 DIS), zeroed, selected, started, with no
  interrupt. PMC1 always counts cycles; PMC2 to PMC4 take one group's events a frame, six groups in
  turn: `ipc` (instructions completed, L1 data misses, mispredicted branches), `dcache` (L1 data miss
  cycles above the threshold, load miss cycles, DTLB search cycles), `l2` (L2 hits, data L2 misses,
  castouts), `branch` (speculative-branch stall cycles, LR/CTR stall cycles, instructions), `mix`
  (dispatched, floating point, integer instructions), `tlb` (ITLB search cycles, DTLB misses, snoop
  retries). The event numbers are in `hwcount.c`. Counted over the frame's whole period (frame begin
  to the next begin: the game, the drawing, the sound), by every thread and interrupt, minus the
  bracketed waits; the 32-bit counters are read as differences modulo 2^32 (they wrap in about six
  seconds at 729 MHz, so a window is far shorter). Dolphin does not model them (they read back what
  was written): the engine checks the cycles against the time base (12 a tick, within 30%) and marks
  the processor's counters invalid, with one log line, after three windows that fail.
- **The GPU.** One pair of GX's metrics a frame (`GX_SetGPMetric(perf0, perf1)`; libogc queues the
  selection in the FIFO and `GX_ClearGPMetric` clears by a register write) in seven groups: the XF/RAS
  group (`GX_InitXfRasMetric`: RAS busy, GP clocks, XF waiting for input, XF waiting to hand on),
  transform clocks with vertices, lighting clocks with texels, triangles with texture-memory stalls,
  quads with texture-cache misses, XF waiting for input with texture-unit idle, the bottom of the XF
  pipe with vertex-cache stalls; and the vertex cache's own check/miss/stall counters every frame.
  Fractions are shares of the GP's clocks: the XF/RAS group measures them (its clocks counter), the
  pairs use the window's time at that measured rate (243 MHz nominal: the Hollywood bus is four times
  the time base). libogc's `GX_ReadGPMetric` never stores through its second pointer (its first
  instruction after the loads overwrites r4: confirmed by disassembling `libogc.a`), so the counters
  are read from the command processor's registers, the addresses its own readers use:
  0xCC000040/44/48/4C (32 bits in two 16-bit halves, read again if the high half moved), the vertex
  cache at 0xCC000050/54/58 and the FIFO's read/write distance at 0xCC000030. Which slot holds
  counter 0 and counter 1 of a pair (0xCC000040 and 0xCC000048 by libogc's reading code) is found by
  the self-test and logged.
- **Self-test.** `wl_hwcount_selftest_begin()` selects the vertex metric on both counters;
  the program draws N vertices, syncs, and calls `wl_hwcount_selftest_end(N)`: the slots that read N
  are the counters, and if none does the GPU counters are marked invalid ("gp: no counters").
  Without that call the engine decides from the windows: a GPU group set that reads all zero over two
  rotations is invalid. Dolphin gives zeros, and the log says so rather than printing nonsense.
- **Timeline.** A mark sends a draw-sync token (the same sequence `wl_gx_sync` uses) and stamps the
  time the processor gave it; the token interrupt (`GX_SetDrawSyncCallback`) stamps the time the GPU
  passed it, and the block shows each mark's mean offset into the frame for both: the gap is how far
  the GPU lags there. A mark flushes the write-gather pipe, so it perturbs the frame a little. The
  FIFO's fill (the GP's read/write distance) is sampled by a tick task at `fifo_hz` and at each mark.
- **The log.** A block a second: the frame's mean and worst time and the processor's waits for the
  GPU and the display (`block`), two `cpu:` lines, two `gp:` lines, `fifo:`, `marks`, and a
  `verdict:`. Medians of the samples of each metric since the last block (about five samples of each
  group a second at 30 frames); a metric not measured shows `-`.
- **The verdict** (`wl_hwcount_verdict`, a pure function of a `WlHwcountView`: the medians, the
  frame's mean time, the waits and the FIFO's share). The frame *fits* when it is within 5% of the
  target and the processor waits for the display at least 10% of it. Else the GP is *starved* (the
  processor is the bound) when most of the witnesses agree: XF waiting for input at least 60% of the
  clocks, the FIFO at most 10% full on average, the processor waiting for the GPU less than 10% of
  the frame. The processor's cause is then named: memory (L1 data miss stall cycles at least 30%),
  branches (stall cycles 15% or 15 mispredicts per 1000 instructions), pipeline stalls (IPC under 0.6
  without those), or compute. Otherwise the GPU is the bound and the part that most exceeds its
  threshold is named: transform (XFRM + lighting clocks 30%), fill/TEV (RAS busy 50% or XF waiting to
  hand on 30%), texture (texture-memory stalls 15%, or 100 texture-cache misses per 1000 texels with
  RAS at least 30% busy), vertex fetch (vertex-cache stalls 15%); *mixed* when none is over. These
  thresholds (`WlHwcountThresholds`) are starting values read off the counters' definitions, not
  tuned on a console: the first runs on one are what set them.
- **Cost.** A window costs a handful of SPR reads and writes and register reads at the frame's start,
  and the FIFO sampler's tick, a few instructions each, at `fifo_hz`. The PMC reads are about ten
  cycles each.
- **Tests.** `tests/platform/test_platform_hwcount.c` drives the engine with a fake machine (the
  register encodings, the modulo arithmetic, medians, the file, the verdict on each kind of frame,
  rotation, waits, marks and tokens, the self-tests, the log). The machine's Wii half
  (`wii/hwcount_wii.c`) is compiled by the Wii build; what it reads is the console's to confirm.

## Console and stop screen (`stop.h`)

A program in bring-up, before its renderer draws, still needs to say what it does and why it
stopped.

- `wl_platform_console_start()` starts libogc's text console in the video's back buffer
  (`wl_platform_video_init` first, if need be), so `printf` shows on the screen. It takes one of the
  video's framebuffers rather than allocating a third (600 KB of MEM1's heap at 640x480); the
  program's frames cover the text once they start. Idempotent; on the host it does nothing and
  stdout is the terminal.
- `wl_platform_console_report(tag, fmt, ...)` writes a line to stdout and to the log.
- `wl_platform_stop_screen(fmt, ...)` does not return. It reports the message and logs it as an
  error, flushes the log, puts the console's framebuffer back on the screen after the last frame
  presented (`wl_platform_video_wait_shown`, then `VIDEO_SetNextFramebuffer`: the retrace
  interrupt would otherwise show the last frame over the text), and waits for HOME on a Remote or
  START on a GameCube controller, or a reset or power event, to leave for the loader. The first stop
  wins; one reached while stopping only waits. On the host it prints to stderr and exits with
  status 1, or calls the hook of `wl_host_stop_set_hook` (a test leaves it with `longjmp`).
- `wl_platform_missing_symbol(name)` is the stop for a function the program does not provide:
  `tools/link_stubs.py` defines every unresolved symbol as a function that calls it.

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

## Frame overlay (`overlay.h`) and audio fade

**Frame overlay.** `wl_gx_set_overlay(fn, user)` hangs a function on the end of every frame:
`wl_gx_end_frame` calls `fn(user, width, height)` after the program's last draw and before the display
copy, so what it draws lands over the finished picture. A menu, a notice or a gauge that belongs to the
platform rather than the program (the HOME button's menu, a battery warning) can be put there without
touching the program's renderer. Rules:

- The state is reset before the call (`wl_gx_reset_state`: the whole EFB, an orthographic projection
  over its pixels with y down, blending off) and after it, so the overlay inherits nothing and leaves
  nothing; a program that sets GX behind the shadow calls `wl_gx_invalidate_shadow` before
  `wl_gx_end_frame` as it must anyway.
- The back buffer has the EFB (a render target that held it has been resolved).
- An unset hook costs one pointer test a frame, and the program that wants the frame free when nothing
  is shown sets the hook only while something is (`wl_gx_set_overlay(NULL, NULL)` removes it). The
  hook is read on the thread that ends frames; set it from there.
- The overlay's draws are in the frame, so its quads cost the GPU what quads cost: a menu's text is
  some hundreds of them (a run of lit pixels in a glyph's row is one quad), sent in batches of 128.

`overlay.h` draws for it with no textures and no assets: `wl_overlay_begin` (blending, no depth, vertex
colours), `wl_overlay_rect`, `wl_overlay_frame`, `wl_overlay_gauge`, `wl_overlay_text` (with a
shadow; `\n` starts a line; a scale of 1 to 8 screen pixels to a glyph pixel), `wl_overlay_text_width`
and `_height`, and `wl_overlay_end`. The font is 5 by 7 pixels in a cell of 6 by 9: capitals, digits
and `. , : ; - + ! ? % / ( ) ' _ > < [ ] = *`; lower case is drawn as capitals and any other character
as a box. Hand-drawn for this library (the glyph shapes are the usual ones for a 5 by 7 matrix).
Tests: `test_platform_overlay` (the hook once a frame with the state reset around it, removable; the
quads of a rectangle, an outline, a gauge and a glyph; batches; the font's glyphs differ).

**Audio fade.** `wl_platform_audio_fade(gain, ms)` sets the output's master gain: from where it is to
`gain` (0 to 1) linearly over `ms` milliseconds of output (0: at once), applied by the audio thread to
each hardware block after the program's `fill` (so a program's mixer is untouched: a pause menu fades
the sound out and back with one call each). While the gain is 1 and no ramp runs, a block costs a
test. Two calls between two blocks make one: the last wins, from where the ramp is. The request is
three atomics, so any thread may call it; `wl_platform_audio_gain()` returns the last target. Test:
`test_platform_audio_fade` (untouched at 1, at once for 0 ms, the ramp's shape and end, continuing from
the middle of a ramp, clamping, set before the output starts).

`wl_overlay_text_drawable(text)` says whether every character of a string has a glyph of its own (lower
case counts, a newline counts), for checking the words a program gives its menus.

## HOME menu and wrist strap reminder (`home_menu.h`, `strap_screen.h`, `home_shell.h`)

What a Wii program that pauses for HOME needs, with nothing of any game in it. Three layers, each usable
without the next.

**`home_menu.h`: the menu and the notices.** `WlHomeMenu` is a state machine with no console in it:
`wl_home_menu_step(menu, raw, extra_nav, home_extra, world, now_us, ops)` is given the controllers each time
the program reads them (`extra_nav` and `home_extra` are for a controller the console does not have, such as
a scripted one) and calls its `WlHomeOps` (pause, sound fade, reset, leave, a visible flag, a log line; each
may be NULL). HOME (a Remote's, or a Classic Controller's own) opens the menu, Resume / Reset / Return to the
loader (asked about first); HOME again or B closes it; the pause is made only if the program was not paused
already; the buttons held as it closes are kept from the program (`wl_home_menu_blocks_input`) until let go
or a time. The notices need no menu: a Remote with no Nunchuk or Classic Controller while a player is on it
in play, and a weak battery (looked at every two seconds, told once per fall, a 0 before the first status
report not counted). What a program may set is in `WlHomeConfig` (`wl_home_config_default()` fills it):

| Field | Meaning |
|---|---|
| `title`, `resume_label`, `loader_label`, `confirm_text`, `cancel_label`, `batteries_heading`, `remote_label`, `hint_menu`, `hint_confirm` | the words (NULL: the default) |
| `reset_label` | NULL: no reset item; else the item is there while `world.in_play`, and `ops.reset` is called after the menu has closed and resumed the program |
| `notice_battery`, `notice_no_extension` | the notices' templates (`{port}` is the Remote's number, `{percent}` the battery); NULL: that notice is never given |
| `classic_swap` | false: the Classic Controller's A accepts and B goes back, as the Wii's menus have them; true: swapped |
| `stick_threshold` | a stick counts as up or down beyond this (64) |
| `fade_ms`, `no_extension_us`, `notice_us`, `battery_check_us`, `swallow_us`, `battery_low`, `battery_good` | the times and levels (0: the default of each: 300 ms, 1.5 s, 7 s, 2 s, 1.5 s, 10 %, 15 %) |

`wl_home_navigation(raw, config)` is what the controllers give the menu, as bits of `WL_HOME_NAV_*` (the
Remote's D-pad, 1, 2, A and B; the Classic Controller's; the stick of a Nunchuk or Classic Controller; a
GameCube pad's). `home_menu_draw.c` draws the menu and the notices with `overlay.h`
(`wl_home_menu_draw`, `wl_home_menu_draw_notices`), between the caller's `wl_overlay_begin` and
`wl_overlay_end`. The text is in the overlay's font; `wl_overlay_text_drawable` checks it.

**`strap_screen.h`: the reminder.** `WlStrapScreen` shows a plain screen at the first look (when enabled):
a title, up to six lines (one of them kept for a Nunchuk, shown only while one is connected) and a prompt
that is dim until the controllers stop being ignored (a second) and lit after. A fresh press of A after
that goes on; an A held from before, or through the second, does not; the program is kept from the A until
it is let go (or 1.5 s). It is shown once. `WlStrapConfig` has the words, `classic_swap` and the two times.
`wl_strap_screen_draw` draws it.

**`home_shell.h`: both on the console.** `WlHomeShell` owns a menu and a reminder and does the console's
part: `wl_platform_audio_fade` for the sound, `wl_platform_exit(WL_EXIT_LOADER)` for the leave (or the
program's own hook), and the overlay (`wl_gx_set_overlay`) hooked only while the menu, a notice or the
reminder shows, so a closed menu costs the frame nothing. The program fills a `WlHomeShellOps`
(`set_paused`, `reset`, `leave_for_loader`, `log`, and two that read the program: `world`, asked for each
look at the menu and not while the reminder is up, and `strap_enabled`, asked once at the first poll, so
that the program's settings can be read late), calls `wl_home_shell_poll(shell, raw, now_us, extra_nav,
home_extra)` each time it reads the controllers, and shows its game no controller while
`wl_home_shell_blocks_input` is true and no rumble while `wl_home_shell_open` is (both safe from any
thread). The reminder takes the controllers first: HOME does nothing under it. The shell owns the
overlay hook and must not be moved once initialised.

Tests: `test_platform_home_menu` (opening and closing, the items, the confirmation, held buttons, each
controller's navigation, the notices' timing and debounce, the configuration, the words drawn),
`test_platform_strap_screen` (the first second, held A, each controller, the words, the configuration),
`test_platform_home_shell` (the overlay hooked and let go, the sound, the program's hooks, the reminder
before the menu), all on the host backend and the big-endian build.

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
| `WL_TAG_GXFIFO` | MEM1 | 1 MiB (the FIFO is `WL_GX_FIFO_BYTES`; with a smaller budget, the largest block it holds, `wl_gx_fifo_bytes`: libogc's examples use 256 KiB, and the GP reads its FIFO from MEM1 on the console) |
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

**Video.** `video_mode.h` (`wl_video_choose`, a pure function, tested over every combination of the
settings) says what the console's settings ask for, and `wl_platform_video_init` applies it to
libogc's own mode object (`VIDEO_GetPreferredMode`, which applies SYSCONF the same way; init logs a
warning if the two differ) and logs the choice once (`[VIDEO] 480p NTSC 59.94 Hz, ...`):

| Setting | Mode |
| --- | --- |
| NTSC or MPAL console | 480i, 59.94 Hz |
| progressive scan on and a component cable | 480p instead (576p at 50 Hz) |
| PAL console with EuRGB60 on | EuRGB60: 480 lines at 59.94 Hz |
| PAL console, EuRGB60 off | 576i at 50 Hz |
| 16:9 | the line the interface scans is 704 pixels, not 640 |
| display position (`CONF_GetDisplayOffsetH`) | libogc adds it to the line's origin and keeps the result on the 720-pixel line (4:3: up to 40 either way; 16:9: 8) |

The picture is always 640x480: the EFB's 480 top rows, in every mode (libogc's PAL mode has a 480-row
EFB and a 576-row XFB, so the display copy scales 480 rows to 576; the EFB's rows below the picture,
to 528, are scratch and are never copied to the screen). A retrace is a field in interlaced modes.
XFBs are cleared to YUV black (0x10801080), not zero (which is green).

**16:9.** The interface stretches the line it scans over the whole screen, so on a 16:9 console
setting a 4:3 picture comes out stretched 33% sideways. `wl_platform_video_set_widescreen` (before
init) chooses: `WL_WIDESCREEN_PILLARBOX` (the default) keeps it 4:3, `WL_WIDESCREEN_STRETCH` fills the
screen. The interface can widen a framebuffer but not narrow one (libogc's `__setScalingRegs` acts
only when the picture is narrower than the line), so the pillarbox is made in the framebuffer:
`wl_gx_end_frame` (after the overlay, before the display copy) copies the finished EFB into the render
targets' colour block, clears the EFB to black in the same copy and draws the block back as one
linearly filtered quad over columns 80 to 560 (`wl_gx_target_squeeze`); the interface then widens the
640-wide framebuffer to 704 and the 480 columns are 3/4 of the screen, 4:3. Cost: one 640x480 RGBA8
EFB copy and one 480x480 quad a frame, the picture 25% less sharp sideways, and the 1.2 MiB block
(the one a park of the back buffer uses, shared with it). Without memory the picture is shown
stretched (one warning). The program draws 4:3 and needs no change; `WlVideoInfo.pillarbox` and
`picture_x`/`picture_width` say it is done. A real anamorphic mode (the program widening its projection
to 16:9 and drawing all 640 columns) would be a third value of the same setting.

**50 Hz.** A frame's stay on the screen is a ratio of retraces to a frame (`wl_present_set_ratio`):
`wl_platform_video_set_interval(2)` is 30 Hz on a 60 Hz display, and 25 Hz on a 50 Hz one. To keep a
rate in frames a second whatever the display, use `wl_platform_video_set_rate(30)`: at 60 Hz two
retraces a frame, at 50 Hz alternately two and one (5 retraces in 3 frames: 30.0 frames a second, with
the unevenness of 3:2 pulldown; `wl_video_rate_ratio`). A program that ticks by the clock keeps its
speed either way; with an interval of two it would show 25 frames a second.

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
The FIFO's peak fill is sampled at each `wl_gx_end_frame` (`WlGxStats.fifo_peak`) and the log says each new peak by 64 KiB steps and at `wl_gx_shutdown`: how much of a smaller `WL_TAG_GXFIFO` budget a program uses. A FIFO overflow or bad state hangs real hardware: the debug validation in `gx_wii.c` catches
bad enums, unaligned buffers and draws with the wrong state. TMEM is about 1 MiB: prefer CMPR
and C4/C8 textures. `wl_gx_copy_efb_texture` is the render-to-texture path; its I4/I8 copies take
the red channel as is (the luminance copy would map black to 16 and white to 235). Copies out of
the EFB to textures are unfiltered: the vertical (deflicker) filter that stays set for the display
copy would blur them, so it is switched off around each copy and back on after. A copy may reach
any of the EFB's 528 rows, below the picture too.

**Render targets** (`gx_target.h`). GX draws into one EFB, so a texture to render into takes it
from the back buffer and gives it back. `wl_gx_target_use(&target)` is called before each draw
(or clear) with the surface it goes to, NULL for the back buffer. A change of surface moves the
EFB: a back buffer with something drawn into it (`wl_gx_target_drawn`) is parked first, its
colour (RGBA8) and depth (Z24X8) copied into two blocks of the layer's own (from the allocator
`wl_gx_target_set_allocator` gives, `aligned_alloc` by default), unfiltered; a target is drawn within a
scissor of its size at the EFB's origin and is resolved when it leaves (its pixels copied into
`target.memory` in GX's RGBA8 or RGB565 tiles, which a texture object over that memory then reads,
and the target is returned to the caller to record); the back buffer is put back by a quad of the
whole of it that writes the parked colour and, through a Z texture (`GX_SetZTexture` replace, depth
test always passing), the parked depth. `wl_gx_target_clear` is a clear of a rectangle once the
frame has begun, also a quad. `wl_gx_begin_frame` gives the EFB to the back buffer, and
`wl_gx_end_frame` puts it back if a target holds it. While a target holds it,
`wl_gx_target_parked_colour` gives the back buffer's parked colour as an RGBA8 texture's image (the
part parked), for a caller that reads the picture it is drawing while it draws into a target.
The target functions that draw use raw GX and
call `wl_gx_invalidate_shadow` after; what they leave set is listed with `wl_gx_target_clear`.
The part of the EFB that is the back buffer defaults to the video mode's width and EFB height and
is set with `wl_gx_target_set_back_buffer_size` (a game that draws a 640 by 480 picture in a taller
EFB parks and restores that). Tests: `test_platform_target` (the owner on the host stub),
`test_platform_target_gx` (`wii/gx_target_wii.c` against a recording stand-in for libogc's GX, the exact calls in
order) and the console suite's `gx_target_*` cases (read back from the EFB).

**The ramp.** `wl_gx_ramp_bind(map)` loads a 256 by 4 I8 texture whose value is its x coordinate
(nearest, clamped) into a texture map; a vertex whose texture coordinate is
`wl_gx_ramp_coordinate(v)` delivers the number `v` (0 to 1) to the TEV as the texture's colour and
alpha. It carries a value that GX has no vertex output for (a vertex shader's fog factor, a weight)
into the stages.

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
A Remote reports about every 5 ms, six or seven times in a 30 Hz frame, and libogc queues the
reports (16 a Remote; `WPAD_DroppedEvents` counts a longer frame's losses). A poll reads all of
them (`WPAD_ReadPending` with a callback, which is what `WPAD_ScanPads` does with none) and folds
them into the one `WlRawRemote` (`input_reports.h`): `buttons` has every button pressed in any
report (a tap shorter than a frame is not lost; it is released in the next frame), the pointer is
the mean of the reports that had one (none if none did), `accel` is the last report's and
`accel_peak` the report of greatest magnitude (a swing, for gestures), `reports` counts them (0:
none came since the last poll, and the last state stands). A report with an error is skipped. The
folding is about 330 instructions a report (counted with `tools/qemu_icount.py` on `ppc-be`), so a
frame of one Remote's seven reports costs a few microseconds. IR is the bounded position when
libogc has a fix, else the unbounded one, so the pointer keeps tracking off-screen.
`wl_pointer_update_steady` adds a play radius that holds a hand's tremor out of the pointer: it
moves only to a reading farther than the radius from where it is held (or at the screen's edge),
and then to the reading itself, so a deliberate move has no lag. The accelerometer is
`WPAD_GForce` x 100. The remote battery byte is 0..200
on the wire, reported as 0..100. Data format and IR resolution are re-applied on every new
connection. A Remote that was connected and reads NOT_READY or TRANSFER (an extension plugged in) stays
connected with nothing pressed for up to 120 polls, then counts as gone. A GameCube controller that
answers `PAD_ERR_TRANSFER` or `PAD_ERR_NOT_READY` (libogc hands it back with nothing pressed) keeps
its last good state, so that a held trigger is not released for a frame, until twenty samples in a row
(a third of a second at 60 Hz) have failed; then it counts as gone, and returns with its next good read.
The GameCube ports are read at every video retrace (`wl_input_wii_retrace`, called from the retrace
interrupt by `video_wii.c`; `WlPadTracker` in `input_reports.h`), 60 or 50 times a second, and a poll
takes what came in since the last one: a button seen pressed in any sample is pressed for the poll, so a
tap shorter than a frame is not missed (it is released at the next poll), and the sticks and triggers
are the latest sample's. The poll reads the pads itself only when no retrace has sampled them since its
last (video not started). A second read inside one field would find no new data and answer
`PAD_ERR_TRANSFER`, so the poll never reads again after a retrace's read. `wl_platform_input_pad_stats`
counts the retrace samples, how many had fresh data, and the polls that read for themselves: on a
console the fresh share should be near all of them (about one in two would mean the pads' own polling
runs at 30 Hz). A Nunchuk's acceleration (`WPADData.exp.nunchuk.gforce`, x 100) of each report is kept
(`WlRawRemote.nunchuk_accel`, the last 16, oldest first, `nunchuk_reports` counting all), and
`input_gesture.h` finds a forward thrust in it: a rise of the forward axis by about 1 g within 130 ms,
from 150 ms of rest, with the other axes quiet (a change, not a size: the Nunchuk clips near 2 g), one
fire per thrust and a hold-off of 600 ms. Tested on synthetic traces (`test_platform_input_gesture`). Sticks are -127..127 over
the real travel of the stick (`input_shape.h`: libogc's `PAD_Clamp` takes a GameCube stick as full at
72, or 59 for the C stick, after a dead zone of 15 per axis, so a raw reading is full at 87 and 74; it
is not called, because the application's own dead zone would add to it); triggers are 0..255: the
Classic Controller's 5 bits scaled, the GameCube's 30..180 (`PAD_Clamp`'s) scaled. The Classic
Controller Pro has no analogue triggers: the digital `WL_CC_L` / `WL_CC_R` bits are the sure sign
that one is pressed (the analogue reading may stay 0), so an application ORs the two. GameCube ports are absent on the Wii Family Edition, Wii mini and vWii.

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
up as OSREPORT. The SD log file is buffered (4 KiB). It is written out and synced to the card (libfat
keeps what is written in its sector cache until the file is synced or closed) when the buffer is full,
at the first line that comes 2 s (`WL_LOG_SYNC_SECONDS`) after the last sync, on `wl_platform_log_flush`
(also called by the stop screen, the profiler and `wl_platform_log_backtrace`) and by `wl_platform_exit`.
A CPU exception's report goes to OSREPORT and the screen and does not write the file: it runs in the
crashed thread, which may hold libfat's lock, and waiting for that would hang the report. So a crash
or a power-off loses the last 2 s of the file's lines, no more. The file opens on a call of
`wl_platform_log_init` with a directory after the SD card is mounted (a second call opens it if the
first had none).

## Dropped from the original code

The original platform layer also carried two game-specific TEV programs (a cel-shading recipe
and a multi-texture character material) with their extra setters; they are not part of this
library. Build such programs on GX directly and call `wl_gx_invalidate_shadow` afterwards.
