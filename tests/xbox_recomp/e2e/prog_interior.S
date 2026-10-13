/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter (tests/xbox_recomp/e2e.py): control flow that crosses the
 * function boundaries a disassembler draws. Hand-written, because a compiler emits none of it on
 * purpose, while a title's CRT and hand-optimised code do:
 *
 *   - a false function start (sw_false, o_false: an address the disassembler took from an
 *     immediate) inside a function, with a loop and a switch whose arms lie on both sides of it;
 *   - a conditional jump (a_fn) and a tail jump (c_fn) into the middle of another function's body;
 *   - a direct call (d_fn) and an indirect call (run) to an address inside a body that no function
 *     starts at (an alternate entry);
 *   - a function whose symbol stops short of its code (s_fn), so it runs off its end into bytes no
 *     function covers.
 *
 * The harness takes every sized symbol for a function, as a function list would, and marks the
 * *_false ones as found from an immediate. run(seed) fills out[] and returns a hash; the image runs
 * natively (the oracle) and lifted, and both must agree. */
        .intel_syntax noprefix
        .text

/* int s_fn(int x): its symbol's size ends before its code does. */
        .globl  s_fn
        .type   s_fn, @function
s_fn:
        mov     eax, [esp+4]
        add     eax, eax
        .size   s_fn, . - s_fn
        xor     eax, 0x3c3c
        ret

/* int sw_sum(int n, int seed): a loop over a switch (its table in .rodata); sw_false splits both. */
        .globl  sw_sum
        .type   sw_sum, @function
sw_sum:
        push    ebx
        push    esi
        mov     ecx, [esp+12]
        mov     ebx, [esp+16]
        xor     esi, esi
        xor     edx, edx
.Lsw_loop:
        cmp     edx, ecx
        jge     .Lsw_done
        mov     eax, ebx
        add     eax, edx
        and     eax, 3
        jmp     [.Lsw_table + eax*4]
.Lsw_arm0:
        add     esi, edx
        jmp     .Lsw_next
        .size   sw_sum, . - sw_sum
        .type   sw_false, @function
sw_false:
        lea     esi, [esi + edx*2 + 1]
        jmp     .Lsw_next
.Lsw_arm2:
        xor     esi, edx
        rol     esi, 5
        jmp     .Lsw_next
.Lsw_arm3:
        imul    esi, esi, 3
.Lsw_next:
        inc     edx
        jmp     .Lsw_loop
.Lsw_done:
        mov     eax, esi
        pop     esi
        pop     ebx
        ret
        .size   sw_false, . - sw_false

/* int b_fn(int x) = core(x * 5), where core is the code from .Lb_core on: an alternate entry. */
        .globl  b_fn
        .type   b_fn, @function
b_fn:
        mov     eax, [esp+4]
        lea     eax, [eax + eax*4]
        jmp     .Lb_body
.Lb_core:
        mov     eax, [esp+4]
.Lb_body:
        push    ebx
        mov     ebx, eax
        imul    ebx, ebx, 7
        xor     ebx, 0x5a5a
        mov     eax, ebx
        pop     ebx
        ret
        .size   b_fn, . - b_fn

/* int a_fn(int x) = x < 0 ? core(x) : x ^ 0x77, the core by a conditional jump into b_fn. */
        .globl  a_fn
        .type   a_fn, @function
a_fn:
        mov     eax, [esp+4]
        test    eax, eax
        jl      .Lb_body
        xor     eax, 0x77
        ret
        .size   a_fn, . - a_fn

/* int c_fn(int x) = core(x + 3), by a tail jump into b_fn's body. */
        .globl  c_fn
        .type   c_fn, @function
c_fn:
        mov     eax, [esp+4]
        add     eax, 3
        jmp     .Lb_body
        .size   c_fn, . - c_fn

/* int d_fn(int x) = core(x - 9) + core(x), by calls to the alternate entry. */
        .globl  d_fn
        .type   d_fn, @function
d_fn:
        push    esi
        mov     eax, [esp+8]
        sub     eax, 9
        push    eax
        call    .Lb_core
        add     esp, 4
        mov     esi, eax
        push    dword ptr [esp+8]
        call    .Lb_core
        add     esp, 4
        add     eax, esi
        pop     esi
        ret
        .size   d_fn, . - d_fn

/* int o_fn(int n, int seed): a loop that crosses the false start o_false both ways: forward into
 * its body (not its start) and back into o_fn's. */
        .globl  o_fn
        .type   o_fn, @function
o_fn:
        push    esi
        push    edi
        mov     ecx, [esp+12]
        mov     eax, [esp+16]
        xor     edi, edi
.Lo_top:
        test    cl, 1
        jnz     .Lo_odd
        add     eax, 0x1234
        rol     eax, 3
        .size   o_fn, . - o_fn
        .type   o_false, @function
o_false:
        xor     eax, edi
        jmp     .Lo_step
.Lo_odd:
        imul    eax, eax, 0x2f
        add     edi, ecx
.Lo_step:
        dec     ecx
        jnz     .Lo_top
        add     eax, edi
        pop     edi
        pop     esi
        ret
        .size   o_false, . - o_false

/* u32 run(u32 seed) */
        .globl  run
        .type   run, @function
run:
        push    ebx
        push    esi
        push    edi
        mov     esi, [esp+16]
        mov     eax, esi
        and     eax, 7
        add     eax, 10
        push    esi
        push    eax
        call    sw_sum
        add     esp, 8
        mov     [out], eax
        push    esi
        call    a_fn
        add     esp, 4
        mov     [out+4], eax
        mov     eax, esi
        or      eax, 1
        neg     eax
        push    eax
        call    a_fn
        add     esp, 4
        mov     [out+8], eax
        push    esi
        call    b_fn
        add     esp, 4
        mov     [out+12], eax
        push    esi
        call    c_fn
        add     esp, 4
        mov     [out+16], eax
        push    esi
        call    d_fn
        add     esp, 4
        mov     [out+20], eax
        mov     ebx, OFFSET .Lb_core
        push    esi
        call    ebx
        add     esp, 4
        mov     [out+24], eax
        mov     eax, esi
        and     eax, 3
        add     eax, 5
        push    esi
        push    eax
        call    o_fn
        add     esp, 8
        mov     [out+28], eax
        mov     eax, esi
        shr     eax, 1
        and     eax, 31
        or      eax, 1
        push    esi
        push    eax
        call    o_fn
        add     esp, 8
        mov     [out+32], eax
        push    esi
        call    s_fn
        add     esp, 4
        mov     [out+36], eax
        mov     eax, esi
        xor     ecx, ecx
.Lrun_hash:
        imul    eax, eax, 31
        add     eax, [out + ecx*4]
        inc     ecx
        cmp     ecx, 10
        jb      .Lrun_hash
        pop     edi
        pop     esi
        pop     ebx
        ret
        .size   run, . - run

        .section .rodata
        .align  4
.Lsw_table:
        .long   .Lsw_arm0, sw_false, .Lsw_arm2, .Lsw_arm3

        .data
        .align  4
        .globl  out
        .type   out, @object
out:
        .zero   64
        .size   out, . - out

        .section .note.GNU-stack, "", @progbits
