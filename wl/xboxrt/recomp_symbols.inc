// SPDX-License-Identifier: MIT
// The data symbols generated code (recomp_types.h) reads, in one list: regs.c defines them from it,
// regs.h declares them and builds the per-thread register file (WlXbCpu) from the WL_XB_REG
// entries, and tests/xboxrt/test_xboxrt_abi.c compiles the list against the generated code's
// header when that header is in the tree, so a type that differs is a compile error rather than a
// silent mismatch at link time. The functions generated code calls are listed in the README.
//
//   WL_XB_REG(type, name, dims, init)     per guest thread: saved and restored when the guest
//                                         lock changes hands (WlXbCpu)
//   WL_XB_GLOBAL(type, name, dims, init)  one for the process
//
// x87 state at reset: control word 0x037F (every exception masked, round to nearest, 64-bit
// precision; the CRT sets its own), condition codes C3 (0x4000).

WL_XB_REG(uint32_t, g_eax, , 0)
WL_XB_REG(uint32_t, g_ecx, , 0)
WL_XB_REG(uint32_t, g_edx, , 0)
WL_XB_REG(uint32_t, g_esp, , 0)
WL_XB_REG(uint32_t, g_ebx, , 0)
WL_XB_REG(uint32_t, g_esi, , 0)
WL_XB_REG(uint32_t, g_edi, , 0)
WL_XB_REG(uint32_t, g_ebp, , 0)
WL_XB_REG(uint32_t, g_seh_ebp, , 0)
WL_XB_REG(uint32_t, g_fs_base, , 0)
WL_XB_REG(int, g_df, , 0)
WL_XB_REG(uint32_t, g_eflags_kept, , 0) // the runtime's (thunk.c, pushfd/popfd): EFLAGS' AC and ID
WL_XB_REG(double, g_fp_stack, [8], {0})
WL_XB_REG(int, g_fp_top, , 0)
WL_XB_REG(uint16_t, g_fp_control_word, , 0x037Fu)
WL_XB_REG(int, g_fp_cmp, , 0)
WL_XB_REG(uint16_t, g_fp_cc, , 0x4000u)
WL_XB_REG(uint32_t, g_mxcsr, , 0x1F80u)
WL_XB_REG(RecompXmm, g_xmm0, , {{0}})
WL_XB_REG(RecompXmm, g_xmm1, , {{0}})
WL_XB_REG(RecompXmm, g_xmm2, , {{0}})
WL_XB_REG(RecompXmm, g_xmm3, , {{0}})
WL_XB_REG(RecompXmm, g_xmm4, , {{0}})
WL_XB_REG(RecompXmm, g_xmm5, , {{0}})
WL_XB_REG(RecompXmm, g_xmm6, , {{0}})
WL_XB_REG(RecompXmm, g_xmm7, , {{0}})
WL_XB_REG(RecompMmx, g_mm0, , {{0}})
WL_XB_REG(RecompMmx, g_mm1, , {{0}})
WL_XB_REG(RecompMmx, g_mm2, , {{0}})
WL_XB_REG(RecompMmx, g_mm3, , {{0}})
WL_XB_REG(RecompMmx, g_mm4, , {{0}})
WL_XB_REG(RecompMmx, g_mm5, , {{0}})
WL_XB_REG(RecompMmx, g_mm6, , {{0}})
WL_XB_REG(RecompMmx, g_mm7, , {{0}})

WL_XB_GLOBAL(ptrdiff_t, g_xbox_mem_offset, , 0)
WL_XB_GLOBAL(uint32_t, g_xbox_code_lo, , 0)
WL_XB_GLOBAL(uint32_t, g_xbox_code_hi, , 0)
WL_XB_GLOBAL(int, g_force_return, , 0)
WL_XB_GLOBAL(volatile uint32_t, g_icall_trace, [16], {0})
WL_XB_GLOBAL(volatile uint32_t, g_icall_trace_idx, , 0)
WL_XB_GLOBAL(volatile uint64_t, g_icall_count, , 0)
