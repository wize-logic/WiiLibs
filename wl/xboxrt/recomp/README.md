# wl/xboxrt/recomp: the runtime header for recompiled Xbox code

`recomp_types.h` is what the C that xboxrecomp's lifter generates includes (`#include
"recomp_types.h"`): the register model, the guest memory macros (`MEM8` .. `MEMD`, `MEMF_SET`,
`MEMD_SET`, `XBOX_PTR`), the x87/SSE/MMX state and the helpers the lifter names. It is upstream's
template (`third_party/xboxrecomp/templates/runtime/recomp_types.h`, MIT, Copyright (c) 2026 sp00nz
and contributors) with WiiLibs' patches applied and these changes, so the generated code is right on
a big-endian host (the Wii's Broadway, and 32-bit PowerPC Linux under qemu-ppc for tests) as well as
a little-endian one:

- guest memory holds the x86's little-endian bytes, read and written through one-member structs with
  `scalar_storage_order("little-endian")` (`lwbrx`/`stwbrx` on PowerPC), `volatile` and `may_alias`;
  float and double stores go through the integer bits (`MEMF_SET`, `MEMD_SET`);
- `XBOX_PTR(a)` is `WL_XB_EA(a)` plus `g_xbox_mem_offset`, or plus nothing with `WL_XB_EA_BASE_ZERO`
  (the Wii). `WL_XB_EA(a) = (a & 0x0FFFFFFF) | ((a >> 3) & 0x10000000)`: user space keeps its
  address, the contiguous window 0x8xxxxxxx and its alias 0xFxxxxxxx land at 0x1xxxxxxx (an
  `rlwinm` and an `rlwimi` on PowerPC);
- `RecompXmm` and `RecompMmx` have little-endian storage, so their lane views overlay as on x86;
- `lock xadd`/`lock cmpxchg` (`RECOMP_ATOMIC_ADD32/CAS32`) byte-swap around a compare-and-swap loop;
- `RECOMP_TLS` is empty: the registers are plain globals;
- `RECOMP_FP_PC` (x87 precision control at 24 bits) rounds inline when the result is a normal float
  and calls the exact slow path, out of line, otherwise;
- the indirect-call trace ring and counter exist only with `WL_RECOMP_ICALL_TRACE`;
- `MMX_CVT_F2I` rounds independently of the host's rounding mode;
- GCC only: `#error` when the compiler lacks `scalar_storage_order` (clang ignores it).

The header is **generated**: `gen_recomp_types.py` patches the vendored pipeline in a scratch
directory and rewrites the template; each edit is anchored on the template's own text and fails
loudly if upstream changes that part. Regenerate after a sync (`python3
wl/xboxrt/recomp/gen_recomp_types.py`) and review the diff; `ctest -R xbox_recomp_header_current`
fails while the committed header and the generator disagree. Generating instead of hand-keeping it
means an upstream fix to a helper arrives with the sync, and the conformance net
(`tests/xbox_recomp/conformance.py`) checks the result against the x86 CPU.

Compile generated code with `-ffp-contract=off -fno-strict-aliasing -w` (and `-Os`; on the Wii also
`-mcpu=750 -meabi -mhard-float -msdata=eabi -DWL_XB_EA_BASE_ZERO`). `wl_add_xbox_title`
(`cmake/WiiLibs.cmake`) does.

## The runtime ABI

What the generated code needs from the program it is linked into. WiiLibs' Xbox runtime
(`WiiLibs::xboxrt`) defines these; `tests/xbox_recomp/title/stub_runtime.c` is a do-nothing stand-in
for link tests. Measured with `nm -u` over every object of a full retail title (Halo, 7,493 functions)
and the synthetic test title, and read from the header's declarations.

### Always

Data (plain globals; `RECOMP_TLS` is empty). The runtime saves and restores all of them when it hands
the guest lock from one guest thread to another.

| Symbol | Type | What |
|---|---|---|
| `g_eax` `g_ecx` `g_edx` `g_ebx` `g_esp` `g_esi` `g_edi` | `uint32_t` | the integer registers (`ebp` is a local in each function) |
| `g_ebp` | `uint32_t` | the frame published to frameless callees |
| `g_seh_ebp` | `uint32_t` | the frame `__SEH_prolog`/`__SEH_epilog` hand over |
| `g_fs_base` | `uint32_t` | linear base of `fs` (the guest thread's TIB); added to every `fs:` address |
| `g_df` | `int` | EFLAGS.DF (string instructions' direction) |
| `g_fp_stack` | `double [8]` | the x87 stack |
| `g_fp_top` | `int` | the x87 TOP |
| `g_fp_control_word` | `uint16_t` | x87 control word (precision and rounding control) |
| `g_fp_cmp` | `int` | last x87 compare: -1, 0, 1, 2 unordered |
| `g_fp_cc` | `uint16_t` | x87 condition codes for `fnstsw` |
| `g_xmm0` .. `g_xmm7` | `RecompXmm` | SSE registers |
| `g_mm0` .. `g_mm7` | `RecompMmx` | MMX registers (declared for every includer, not only generated code) |
| `g_xbox_code_lo` `g_xbox_code_hi` | `uint32_t` | bounds of the title's executable sections (`RECOMP_ICALL_IS_CODE`; 0 = allow all) |
| `g_xbox_mem_offset` | `ptrdiff_t` | host address of guest effective address 0 (host and ppc-be; not referenced with `WL_XB_EA_BASE_ZERO`) |

Functions:

| Symbol | What |
|---|---|
| `recomp_func_t recomp_lookup_manual(uint32_t va)` | the title's hand-written functions by guest address, tried first on every indirect call; `NULL` when none (a title overrides it: make the runtime's default weak, or a hook) |
| `recomp_func_t recomp_lookup_kernel(uint32_t va)` | kernel thunks (synthetic addresses 0xFE0000xx and up) |
| `void recomp_icall_fail_log(uint32_t va)` | an indirect call to code with no translation |
| `void recomp_icall_not_code_log(uint32_t va)` | an indirect call to an address that is not code (rate-limit it) |
| `void recomp_debug_service(uint32_t service, uint32_t arg_va)` | `int 0x2d`, the kernel debug trap (service 1 prints the ANSI_STRING at `arg_va`) |
| `void recomp_unimpl(const char *text, uint32_t va)` | an instruction the lifter could not translate was reached |
| `uint64_t xbox_ReadTimeStampCounter(void)` | `rdtsc`, at the Xbox's 733.33 MHz |
| `volatile uint32_t g_xb_attention`, `void wl_xb_safe_point(void)` | `RECOMP_BACKEDGE()` before every backward jump (patch 0008): when the flag is set (another guest thread or interrupt work waits for the guest lock) the loop gives way at its back edge |

From the C library and libgcc: `memcpy`, `memset`, `calloc` (only `recomp_dispatch_init`), libm
(`sqrt`, `floor`, `floorf`, `ceil`, `ceilf`, `trunc`, `truncf`, `fmod`, `frexp`, `ldexp`, `log`, `exp2`,
`sin`, `cos`, `tan`, `atan2`), libgcc's 64-bit helpers (`__divdi3`, `__udivdi3`, `__moddi3`,
`__umoddi3`, `__fixdfdi`, `__floatdidf`, `__floatundidf`) and, at `-Os`, its register save/restore
routines (`_savegpr_*`, `_restgpr_*_x`, `_savefpr_*`, `_restfpr_*_x`).

### Only when used

| Symbol | Needed when |
|---|---|
| `jmp_buf *recomp_setjmp_slot(uint32_t buf_va)`, `int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)` | the lifter found the title's CRT `setjmp`/`longjmp` (Halo's retail XBE: not found) |
| `int g_force_return` | the title config passes `--force-return` |
| `void recomp_trace_enter(const char *, uint32_t)`, `recomp_trace_exit(...)`, `void recomp_trace_esp(const char *, const char *)` | the title config passes `trace_functions` |
| `volatile uint32_t g_icall_trace[16]`, `volatile uint32_t g_icall_trace_idx`, `volatile uint64_t g_icall_count` | built with `WL_RECOMP_ICALL_TRACE` |
| `void recomp_abi_violation_log(...)` | built with `RECOMP_ABI_CHECK` |
| `void recomp_call_profile_hit(uint32_t)` | built with `RECOMP_CALL_PROFILE` |
| `g_icall_guard_hits`, `g_icall_guard_misses`, `recomp_icall_feedback.h` | built with `RECOMP_ICALL_FEEDBACK` |

### Defined by the generated code, for the runtime

`recomp_dispatch.c` defines `recomp_func_t recomp_lookup(uint32_t va)` (binary search over a sorted
table, 8 bytes per function), `int recomp_dispatch_init(void)` (an optional flat table of one pointer
per byte of the code span: 7 MB for Halo, so the Wii should not call it), `size_t
recomp_dispatch_flat_bytes(void)`, `size_t recomp_get_count(void)` and `size_t recomp_call_all(void)`.
Every guest function is `void sub_XXXXXXXX(void)` (or its recovered name; the XBE entry point is
`xbe_entry_point`), declared in `recomp_funcs.h`, with its arguments on the guest stack and the guest
return address pushed by the caller.

### What the runtime must agree on

- the same `WL_XB_EA` transform for its memory map, and `WL_XB_EA_BASE_ZERO` on the Wii (defined by
  `wl_add_xbox_title` for the generated code and the title's own sources);
- guest memory little-endian, accessed with the same macros (or `wl_rd_le*`) in the runtime's own code;
- the register file above is one guest thread's state: plain globals, swapped on a thread handoff.
