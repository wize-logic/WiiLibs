/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter (tests/xbox_recomp/e2e.py): ebx, esi and edi across calls,
 * for --keep-preserved (preserve.py), which leaves them in the caller's locals after a call to a
 * function it proved gives them back. Hand-written, because each case breaks the calling
 * convention on purpose, as a title's CRT and hand-written helpers may:
 *
 *   - saver keeps the convention (pushes and pops what it uses), and so does a stdcall callee
 *     (ret 8) and a recursive one;
 *   - clob changes ebx and does not give it back;
 *   - prolog3/epilog3 save the caller's registers in the caller's frame and restore them from it,
 *     as the SEH prolog and epilog do;
 *   - an indirect call to saver;
 *   - argw stores over its arguments, onto a slot where its caller saved ebx;
 *   - mid saves ebx, and the frameless helper it calls (frw) stores through the frame it
 *     inherits (its caller's caller's ebp) onto that saved ebx;
 *   - fpo, built without a frame pointer, uses ebp as a general register: a pointer loaded into
 *     it and moved through ecx, and stores through it, which are not stores into a frame.
 *
 * run(seed) fills out[] with the registers after each call and returns a hash; the image runs
 * natively (the oracle) and lifted, and both must agree. */
        .intel_syntax noprefix
        .text

/* saver(): changes ebx, esi and edi and gives them back. */
        .globl  saver
        .type   saver, @function
saver:
        push    ebx
        push    esi
        push    edi
        mov     ebx, 0x11
        mov     esi, 0x22
        mov     edi, 0x33
        lea     eax, [ebx + esi]
        add     eax, edi
        pop     edi
        pop     esi
        pop     ebx
        ret
        .size   saver, . - saver

/* int std2(int a, int b), stdcall: gives back esi. */
        .globl  std2
        .type   std2, @function
std2:
        push    esi
        mov     esi, [esp+8]
        mov     eax, [esp+12]
        imul    eax, esi
        pop     esi
        ret     8
        .size   std2, . - std2

/* int rec(int n): n + (n-1) + ... + 1 through esi, which it gives back at every depth. */
        .globl  rec
        .type   rec, @function
rec:
        push    esi
        mov     esi, [esp+8]
        xor     eax, eax
        test    esi, esi
        jz      .Lrec_done
        lea     eax, [esi-1]
        push    eax
        call    rec
        add     esp, 4
        add     eax, esi
.Lrec_done:
        pop     esi
        ret
        .size   rec, . - rec

/* clob(int x): leaves x ^ 0x55 in ebx. */
        .globl  clob
        .type   clob, @function
clob:
        mov     ebx, [esp+4]
        xor     ebx, 0x55
        mov     eax, ebx
        ret
        .size   clob, . - clob

/* prolog3: pushes ebx, esi and edi onto its caller's stack, under the return address. */
        .globl  prolog3
        .type   prolog3, @function
prolog3:
        pop     ecx
        push    ebx
        push    esi
        push    edi
        push    ecx
        ret
        .size   prolog3, . - prolog3

/* epilog3: pops them back from there. */
        .globl  epilog3
        .type   epilog3, @function
epilog3:
        pop     ecx
        pop     edi
        pop     esi
        pop     ebx
        push    ecx
        ret
        .size   epilog3, . - epilog3

/* withprolog(): saves through prolog3, changes the registers, calls saver, restores through epilog3. */
        .globl  withprolog
        .type   withprolog, @function
withprolog:
        call    prolog3
        mov     ebx, 0x101
        mov     esi, 0x202
        mov     edi, 0x303
        call    saver
        add     eax, ebx
        call    epilog3
        ret
        .size   withprolog, . - withprolog

/* argw(int a): stores over its argument and the word above it. */
        .globl  argw
        .type   argw, @function
argw:
        mov     eax, [esp+4]
        mov     dword ptr [esp+4], 0x7777
        mov     dword ptr [esp+8], 0x8888
        ret
        .size   argw, . - argw

/* argcaller(): saves ebx, then calls argw, which overwrites that slot; returns the ebx it pops. */
        .globl  argcaller
        .type   argcaller, @function
argcaller:
        push    ebx
        push    5
        call    argw
        add     esp, 4
        pop     ebx
        mov     eax, ebx
        ret
        .size   argcaller, . - argcaller

/* frw: no frame of its own; stores through ebp, its caller's caller's frame. */
        .globl  frw
        .type   frw, @function
frw:
        mov     dword ptr [ebp-12], 0x4444
        ret
        .size   frw, . - frw

/* mid: saves ebx, changes it, calls frw, which overwrites the saved copy; pops it back. */
        .globl  mid
        .type   mid, @function
mid:
        push    ebx
        mov     ebx, 9
        call    frw
        pop     ebx
        ret
        .size   mid, . - mid

/* framed(int x): a frame, ebx = x, calls mid (whose saved ebx sits at ebp-12); returns ebx. */
        .globl  framed
        .type   framed, @function
framed:
        push    ebp
        mov     ebp, esp
        push    ebx
        mov     ebx, [ebp+8]
        call    mid
        mov     eax, ebx
        pop     ebx
        pop     ebp
        ret
        .size   framed, . - framed

/* int fpo(int *p): frameless, ebp a data pointer; p[1] = p[0] + 5, p[2] = p[1] ^ esi; gives back esi */
        .globl  fpo
        .type   fpo, @function
fpo:
        push    ebp
        push    esi
        mov     ebp, [esp+12]
        mov     esi, [ebp]
        add     esi, 5
        mov     [ebp+4], esi
        mov     ecx, ebp
        mov     ebp, ecx
        xor     esi, 0x5A5A
        mov     [ebp+8], esi
        lea     eax, [esi + 1]
        pop     esi
        pop     ebp
        ret
        .size   fpo, . - fpo

/* int run(int seed) */
        .globl  run
        .type   run, @function
run:
        push    ebx
        push    esi
        push    edi
        mov     ebx, [esp+16]
        lea     esi, [ebx + 0x1234]
        lea     edi, [ebx*2 + 7]
        call    saver
        mov     [out], ebx
        mov     [out+4], esi
        mov     [out+8], edi
        add     ebx, eax
        push    esi
        push    ebx
        call    std2
        mov     [out+12], eax
        mov     [out+16], esi
        mov     eax, ebx
        and     eax, 15
        push    eax
        call    rec
        add     esp, 4
        mov     [out+20], eax
        mov     [out+24], esi
        mov     [out+28], ebx
        push    edi
        call    clob
        add     esp, 4
        mov     [out+32], ebx
        mov     [out+36], esi
        call    withprolog
        mov     [out+40], eax
        mov     [out+44], ebx
        mov     [out+48], esi
        mov     [out+52], edi
        mov     eax, OFFSET saver
        add     esi, 3
        call    eax
        mov     [out+56], esi
        mov     [out+60], ebx
        call    argcaller
        mov     [out+64], eax
        mov     [out+68], ebx
        push    esi
        call    framed
        add     esp, 4
        mov     [out+72], eax
        mov     [out+76], edi
        mov     [fbuf], ebx
        push    OFFSET fbuf
        call    fpo
        add     esp, 4
        mov     [out+80], eax
        mov     [out+84], esi
        mov     eax, [fbuf+4]
        mov     [out+88], eax
        mov     eax, [fbuf+8]
        mov     [out+92], eax
        xor     eax, eax
        xor     ecx, ecx
.Lrun_hash:
        imul    eax, eax, 31
        add     eax, [out + ecx*4]
        inc     ecx
        cmp     ecx, 24
        jb      .Lrun_hash
        pop     edi
        pop     esi
        pop     ebx
        ret
        .size   run, . - run

        .data
        .align  4
        .globl  out
        .type   out, @object
out:
        .zero   96
        .size   out, . - out
        .globl  fbuf
        .type   fbuf, @object
fbuf:
        .zero   16
        .size   fbuf, . - fbuf

        .section .note.GNU-stack, "", @progbits
