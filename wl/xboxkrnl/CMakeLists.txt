# SPDX-License-Identifier: MIT
# wl_xboxkrnl: the Xbox kernel for recompiled titles (xboxkrnl.h), portable C over wl/xboxrt's guest
# memory, threads and interrupts and wl/platform's files.
add_library(wl_xboxkrnl STATIC
    kernel.c ob.c kdata.c ke_wait.c ke_irq.c ke.c ex.c mm.c ps.c rtl.c hal.c xc.c io.c path.c nt_file.c read_ahead.c nt_sync.c)
wl_target_defaults(wl_xboxkrnl)
target_link_libraries(wl_xboxkrnl PUBLIC wl_xboxrt wl_platform wl_base)
add_library(WiiLibs::xboxkrnl ALIAS wl_xboxkrnl)
