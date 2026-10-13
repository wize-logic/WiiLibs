/* SPDX-License-Identifier: MIT
 * The e2e title's layout: an XBE's (image base 0x10000, headers on the first page, code from
 * 0x11000), one page-aligned section for each kind, the thunk table on a page of its own. */
ENTRY(title_entry)
SECTIONS
{
    . = 0x11000;
    .text : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rdata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .thunks : { *(.thunks) }
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.igot*) }
}
