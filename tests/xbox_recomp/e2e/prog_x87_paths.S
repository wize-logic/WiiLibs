/* SPDX-License-Identifier: MIT */
/* An x86 test program for the lifter (tests/xbox_recomp/e2e.py): x87 code whose stack depth has to
 * be followed through control flow, the case the --x87-locals pass (the stack in C locals inside a
 * function, memory current wherever control leaves it) must get right. Hand-written, because a
 * compiler keeps the x87 stack empty across calls and balanced at joins, and hand-written code
 * (a 2001 title's maths library, its CRT's helpers) does not have to:
 *
 *   - depth through branches that join, and a loop that carries values across its back edge;
 *   - calls made with values live on the stack: a callee that reads the caller's st(1), one that
 *     takes st(0) as its argument and pops it, one that returns a value only on some paths (the
 *     caller's paths then join at different depths), an ordinary call between live values and an
 *     indirect call;
 *   - fxch, fld/fst/fstp st(i), the reversed and popping arithmetic forms, fchs, fabs, fsqrt;
 *   - integer loads and stores (fild, fi* arithmetic, fist/fistp at 16, 32 and 64 bits) under each
 *     rounding mode set with fldcw, frndint, and the integer indefinite of an out-of-range value;
 *   - compares read through fnstsw ax with sahf or test ah, fcomip/fucomip with jcc, ftst, fxam,
 *     NaN operands;
 *   - arithmetic at precision control 24 bits (the Xbox's own setting).
 *
 * run(seed) fills out[] and returns a hash; it runs natively (the oracle, at precision control 53
 * bits) and lifted, and both must agree. */
        .intel_syntax noprefix
        .text

/* float branchy(int c, float a, float b): two values through an if/else that join */
        .globl  branchy
        .type   branchy, @function
branchy:
        fld     dword ptr [esp+8]
        fld     dword ptr [esp+12]
        test    byte ptr [esp+4], 1
        jz      .Lbr_else
        fmul    st, st(1)
        fadd    st, st
        jmp     .Lbr_join
.Lbr_else:
        fsubr   st, st(1)
        fld1
        faddp   st(1), st
.Lbr_join:
        fxch    st(1)
        fstp    st(0)
        ret
        .size   branchy, . - branchy

/* double poly(int n, double x): acc = acc * x + 0.5, n times; acc and x live across the back edge */
        .globl  poly
        .type   poly, @function
poly:
        fld     qword ptr [esp+8]
        fld1
        mov     ecx, [esp+4]
.Lpo_loop:
        test    ecx, ecx
        jz      .Lpo_done
        fmul    st, st(1)
        fadd    qword ptr [k_half]
        dec     ecx
        jmp     .Lpo_loop
.Lpo_done:
        fstp    st(1)
        ret
        .size   poly, . - poly

/* scale_top: st(0) = st(0) * 2 + st(1), reading its caller's st(1); no arguments, nothing pushed */
        .globl  scale_top
        .type   scale_top, @function
scale_top:
        fadd    st, st
        fld     st(1)
        faddp   st(1), st
        ret
        .size   scale_top, . - scale_top

/* store_pop(float *p): stores the caller's st(0) and pops it (an argument in st(0)) */
        .globl  store_pop
        .type   store_pop, @function
store_pop:
        mov     eax, [esp+4]
        fstp    dword ptr [eax]
        ret
        .size   store_pop, . - store_pop

/* maybe_push(int c): pushes 1.0 when c is odd, nothing otherwise; returns c & 1 in eax */
        .globl  maybe_push
        .type   maybe_push, @function
maybe_push:
        mov     eax, [esp+4]
        and     eax, 1
        jz      .Lmp_out
        fld1
.Lmp_out:
        ret
        .size   maybe_push, . - maybe_push

/* calls(int s): values live across each kind of call; results to out[0..5] */
        .globl  calls
        .type   calls, @function
calls:
        push    ebx
        mov     ebx, [esp+8]
        fild    dword ptr [esp+8]           /* s */
        fld     dword ptr [k_three]         /* 3, s */
        call    scale_top                   /* 6 + s, s */
        fstp    dword ptr [out+0]           /* s */
        fld     dword ptr [k_quarter]       /* 0.25, s */
        push    offset out+4
        call    store_pop                   /* s */
        add     esp, 4
        /* an ordinary call between live values: branchy pushes and pops above them */
        fld     dword ptr [k_three]         /* 3, s */
        push    dword ptr [k_quarter]
        push    dword ptr [k_three]
        push    ebx
        call    branchy                     /* r, 3, s */
        add     esp, 12
        faddp   st(1), st                   /* r + 3, s */
        fmulp   st(1), st                   /* (r + 3) * s */
        fstp    dword ptr [out+8]
        /* a value returned on one path only: the paths join at different depths */
        fld     dword ptr [k_quarter]       /* 0.25 */
        push    ebx
        call    maybe_push                  /* (1, ) 0.25 */
        add     esp, 4
        test    eax, eax
        jz      .Lca_none
        faddp   st(1), st                   /* 1.25 */
.Lca_none:
        fstp    dword ptr [out+12]
        /* an indirect call with a value live */
        fld     dword ptr [k_three]
        fld     dword ptr [k_quarter]       /* 0.25, 3 */
        mov     eax, ebx
        and     eax, 1
        call    [fn_table + eax*4]          /* scale_top: 3.5, 3 ; or nothing pushed: still 0.25, 3 */
        fsubp   st(1), st
        fstp    dword ptr [out+16]
        pop     ebx
        ret
        .size   calls, . - calls

/* nop_fn: an indirect-call target that leaves the x87 alone */
        .globl  nop_fn
        .type   nop_fn, @function
nop_fn:
        ret
        .size   nop_fn, . - nop_fn

/* shuffle(double *v): four values permuted with fxch and the st(i) forms, written back in order */
        .globl  shuffle
        .type   shuffle, @function
shuffle:
        mov     eax, [esp+4]
        fld     qword ptr [eax]
        fld     qword ptr [eax+8]
        fld     qword ptr [eax+16]
        fld     qword ptr [eax+24]         /* d c b a */
        fxch    st(3)                      /* a c b d */
        fxch    st(1)                      /* c a b d */
        fxch    st(2)                      /* b a c d */
        fld     st(2)                      /* c b a c d */
        fsubr   st, st(1)                  /* b-c b a c d */
        fst     st(4)                      /* b-c b a c b-c */
        fdivr   st(2), st                  /* b-c b (b-c)/a c b-c */
        fsubrp  st(1), st                  /* (b-c)-b ... */
        fchs
        fabs
        fsqrt
        fmul    st, st(3)
        fstp    qword ptr [eax]
        fstp    qword ptr [eax+8]
        fdivr   st(1), st
        fstp    qword ptr [eax+16]
        fstp    qword ptr [eax+24]
        ret
        .size   shuffle, . - shuffle

/* ints(int a): fild at each width, the fi* forms, fistp at each width */
        .globl  ints
        .type   ints, @function
ints:
        mov     eax, [esp+4]
        fild    word ptr [k_w]
        fiadd   dword ptr [esp+4]
        fimul   word ptr [k_w]
        fild    qword ptr [k_q]
        fisub   dword ptr [esp+4]
        fidivr  dword ptr [k_d]
        faddp   st(1), st
        fist    word ptr [out+20]
        fistp   qword ptr [out+24]
        fild    dword ptr [esp+4]
        fisubr  word ptr [k_w]
        fidiv   word ptr [k_w]
        fistp   dword ptr [out+32]
        ret
        .size   ints, . - ints

/* rounding(double v, int *o): fist/fistp/frndint under each rounding mode; 12 words */
        .globl  rounding
        .type   rounding, @function
rounding:
        push    esi
        sub     esp, 8
        mov     esi, [esp+24]
        fnstcw  word ptr [esp]
        xor     ecx, ecx
.Lro_mode:
        movzx   eax, word ptr [esp]
        and     eax, 0xF3FF
        mov     edx, ecx
        shl     edx, 10
        or      eax, edx
        mov     [esp+4], ax
        fldcw   word ptr [esp+4]
        fld     qword ptr [esp+16]
        fist    dword ptr [esi]
        fld     st(0)
        frndint
        fistp   dword ptr [esi+4]
        fadd    dword ptr [k_three]
        fistp   word ptr [esi+8]
        add     esi, 12
        inc     ecx
        cmp     ecx, 4
        jb      .Lro_mode
        fldcw   word ptr [esp]
        add     esp, 8
        pop     esi
        ret
        .size   rounding, . - rounding

/* compares(double a, double b, u32 *o): the compare idioms' answers, three words */
        .globl  compares
        .type   compares, @function
compares:
        push    ebx
        mov     edx, [esp+24]
        /* fcomp, fnstsw ax, sahf, then the flags as setcc reads them */
        fld     qword ptr [esp+8]
        fcomp   qword ptr [esp+16]
        fnstsw  ax
        sahf
        seta    bl
        setp    bh
        setb    cl
        sete    ch
        movzx   ebx, bx
        movzx   ecx, cx
        shl     ecx, 16
        or      ebx, ecx
        mov     [edx], ebx
        /* fucompp, fnstsw ax, test ah and a jump: is it a NaN, a <= b, a < b */
        xor     ebx, ebx
        fld     qword ptr [esp+16]
        fld     qword ptr [esp+8]
        fucompp
        fnstsw  ax
        test    ah, 0x44
        jp      .Lco_1
        or      ebx, 1
.Lco_1:
        test    ah, 0x41
        jne     .Lco_2
        or      ebx, 2
.Lco_2:
        test    ah, 0x05
        jp      .Lco_3
        or      ebx, 4
.Lco_3:
        /* fucomip and fcomip: EFLAGS directly */
        fld     qword ptr [esp+16]
        fld     qword ptr [esp+8]
        fucomip st, st(1)
        fstp    st(0)
        seta    al
        setne   ah
        setp    cl
        movzx   eax, ax
        movzx   ecx, cl
        shl     ecx, 16
        or      eax, ecx
        shl     eax, 4
        or      ebx, eax
        fld     qword ptr [esp+8]
        fld     qword ptr [esp+16]
        fcomip  st, st(1)
        fstp    st(0)
        jae     .Lco_4
        or      ebx, 0x1000000
.Lco_4:
        mov     [edx+4], ebx
        /* ftst and fxam: the condition codes */
        fld     qword ptr [esp+8]
        ftst
        fnstsw  ax
        fstp    st(0)
        and     eax, 0x4500
        mov     ebx, eax
        fld     qword ptr [esp+16]
        fxam
        fnstsw  ax
        fstp    st(0)
        and     eax, 0x4700
        shl     eax, 16
        or      ebx, eax
        mov     [edx+8], ebx
        pop     ebx
        ret
        .size   compares, . - compares

/* pc24(float a, float b, double *o): arithmetic at precision control 24 bits, kept as doubles */
        .globl  pc24
        .type   pc24, @function
pc24:
        sub     esp, 8
        fnstcw  word ptr [esp]
        movzx   eax, word ptr [esp]
        and     eax, 0xFCFF                /* PC = 00: 24 bits */
        mov     [esp+4], ax
        fldcw   word ptr [esp+4]
        mov     eax, [esp+20]
        fld     dword ptr [esp+12]
        fld     dword ptr [esp+16]
        fld     st(1)
        fdiv    st, st(1)
        fstp    qword ptr [eax]
        fld     st(0)
        fmul    st, st(2)
        fadd    dword ptr [k_three]        /* a float: every input has 24 bits */
        fstp    qword ptr [eax+8]
        fsubp   st(1), st
        fabs
        fsqrt
        fstp    qword ptr [eax+16]
        fldcw   word ptr [esp]
        add     esp, 8
        ret
        .size   pc24, . - pc24

/* deep(double x): seven values on the stack at once */
        .globl  deep
        .type   deep, @function
deep:
        fld     qword ptr [esp+4]
        fld     st(0)
        fmul    st, st(1)
        fld     st(0)
        fmul    st, st(2)
        fld     st(0)
        fmul    st, st(3)
        fld     st(0)
        fmul    st, st(4)
        fld1
        fld     dword ptr [k_three]        /* not fldpi: its 64-bit pi is not a double */
        faddp   st(2), st
        fdivp   st(1), st
        faddp   st(1), st
        faddp   st(1), st
        faddp   st(1), st
        faddp   st(1), st
        ret
        .size   deep, . - deep

/* u32 run(u32 seed) */
        .globl  run
        .type   run, @function
run:
        push    ebx
        push    esi
        push    edi
        mov     esi, [esp+16]
        /* branchy, both arms */
        push    dword ptr [k_quarter]
        push    dword ptr [k_three]
        push    esi
        call    branchy
        add     esp, 12
        fstp    dword ptr [out+36]
        mov     eax, esi
        inc     eax
        push    dword ptr [k_quarter]
        push    dword ptr [k_three]
        push    eax
        call    branchy
        add     esp, 12
        fstp    dword ptr [out+40]
        /* poly */
        mov     eax, esi
        and     eax, 7
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        and     esi, 0x7FFFFFFF
        mov     ecx, esi
        and     ecx, 7
        add     ecx, 1
        push    ecx
        call    poly
        add     esp, 12
        fstp    qword ptr [out+44]
        /* calls */
        mov     eax, esi
        and     eax, 255
        push    eax
        call    calls
        add     esp, 4
        /* shuffle */
        mov     eax, esi
        and     eax, 3
        lea     edi, [vals+eax*8]
        mov     ecx, 0
.Lrun_copy:
        mov     edx, [edi+ecx*4]
        mov     [buf+ecx*4], edx
        inc     ecx
        cmp     ecx, 8
        jb      .Lrun_copy
        push    offset buf
        call    shuffle
        add     esp, 4
        mov     ecx, 0
.Lrun_back:
        mov     edx, [buf+ecx*4]
        mov     [out+52+ecx*4], edx
        inc     ecx
        cmp     ecx, 8
        jb      .Lrun_back
        /* ints */
        mov     eax, esi
        and     eax, 0xFFFF
        sub     eax, 0x8000
        push    eax
        call    ints
        add     esp, 4
        /* rounding of one value, picked by the seed */
        mov     eax, esi
        shr     eax, 3
        and     eax, 7
        push    offset out+84
        push    dword ptr [rvals+4+eax*8]
        push    dword ptr [rvals+eax*8]
        call    rounding
        add     esp, 12
        /* compares: (a, b) and (b, a) from the value table, then against a NaN */
        mov     eax, esi
        and     eax, 7
        mov     ecx, esi
        shr     ecx, 4
        and     ecx, 7
        push    offset out+132
        push    dword ptr [vals+4+ecx*8]
        push    dword ptr [vals+ecx*8]
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        call    compares
        add     esp, 20
        mov     eax, esi
        and     eax, 7
        push    offset out+144
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        call    compares
        add     esp, 20
        push    offset out+156
        push    dword ptr [k_nan+4]
        push    dword ptr [k_nan]
        mov     eax, esi
        and     eax, 7
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        call    compares
        add     esp, 20
        /* pc24 */
        mov     eax, esi
        and     eax, 3
        push    offset out+168
        push    dword ptr [fvals+4+eax*4]
        push    dword ptr [fvals+eax*4]
        call    pc24
        add     esp, 12
        /* deep */
        mov     eax, esi
        and     eax, 7
        push    dword ptr [vals+4+eax*8]
        push    dword ptr [vals+eax*8]
        call    deep
        add     esp, 8
        fstp    qword ptr [out+192]
        /* rounding of a second value: the table's other half */
        mov     eax, esi
        shr     eax, 3
        xor     eax, 4
        and     eax, 7
        push    offset out+200
        push    dword ptr [rvals+4+eax*8]
        push    dword ptr [rvals+eax*8]
        call    rounding
        add     esp, 12
        /* the hash */
        mov     eax, [esp+16]
        xor     ecx, ecx
.Lrun_hash:
        imul    eax, eax, 31
        add     eax, [out + ecx*4]
        inc     ecx
        cmp     ecx, 62
        jb      .Lrun_hash
        pop     edi
        pop     esi
        pop     ebx
        ret
        .size   run, . - run

        .section .rodata
        .align  8
k_half:     .double 0.5
k_q:        .quad   -1234567890123
k_nan:      .long   0, 0x7FF80000
vals:       .double 1.5, -2.25, 3.0e10, 0.1, -7.0, 1.0e-3, 2.0, -0.0, 5.5, 1.25, -3.0
rvals:      .double 2.5, -2.5, 3.5, -0.5, 1.0e10, 2147483647.5, -32768.4, 0.49999999999999994
fvals:      .float  1.0, 3.0, 0.7, 1.0e-3, 12345.678
k_three:    .float  3.0
k_quarter:  .float  0.25
k_d:        .long   1000003
k_w:        .short  -77
        .align  4
fn_table:   .long   nop_fn, scale_top

        .data
        .align  8
        .globl  out
        .type   out, @object
out:
        .zero   256
        .size   out, . - out
buf:
        .zero   32

        .section .note.GNU-stack, "", @progbits
