#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""xboxrecomp's conformance cases, lifted, run big-endian, against the x86 CPU they came from.

Each case in xboxrecomp's tools/conformance/cases.py (WiiLibs' patches add some) is a few x86
instructions with a list of inputs. Here:

  1. the cases are assembled with `gcc -m32` (harness.native_source, GNU syntax) and the bytes
     between the nop markers are lifted with the patched pipeline's own lifter (the conformance
     runner's _lift: lift_basic_block, as recomp lifts a block);
  2. the same instructions run natively on this machine's x86 CPU for every input -- the oracle --
     and a recording ("golden" file) keeps what the hardware produced: eax, the x87 status word and
     stack, all eight XMM registers, and the 64-byte scratch buffer before and after;
  3. the lifted C is compiled with WiiLibs' runtime header (wl/xboxrt/recomp/recomp_types.h) for
     big-endian PowerPC Linux (powerpc-linux-gnu-gcc, -Os, the generated code's flags) and run under
     qemu-ppc, replaying the recording: same inputs, guest memory at guest addresses through
     XBOX_PTR, and every field compared (the scratch buffer too, which upstream's harness does
     not: it is how a wrong store shows);
  4. the same replay is built for the host (x86-64), where the header's byte-order attribute is a
     no-op.

Every case must pass, or be in EXPECTED with the reason. A case in EXPECTED that passes fails the
run too, so the list cannot go stale.

    conformance.py --build DIR                 # both targets
    conformance.py --build DIR --target ppc-be -k fpu_   # cases whose name contains fpu_
    conformance.py --build DIR --header upstream          # upstream's own header (to see the net bite)
    conformance.py --build DIR --unpatched                # upstream's lifter, without WiiLibs' patches

Exit status: 0 all good, 1 a case failed (or passed unexpectedly), 77 a needed tool is missing
(gcc -m32, objdump, powerpc-linux-gnu-gcc, qemu-ppc), 2 the harness itself broke.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

WL_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WL_ROOT / "tools"))
import xbox_recomp  # noqa: E402

SKIP = 77

# Cases that do not match the x86 CPU on the big-endian build, each with the reason. Keep it empty
# unless the difference is understood and accepted; a listed case that passes fails the run.
EXPECTED: dict[str, str] = {
    "fpu_ptan": "x87 fptan reduces a large argument (1e10) with a 66-bit pi; libm (glibc here, newlib on "
                "the Wii) reduces it exactly. 2e-11 relative at that input, beyond the case's 1e-15; "
                "the same on the x86-64 build. Hardware vs libm, not a lifting or byte-order error",
    "fpu_sincos_pair": "the same for fsin/fcos of 1e10 (x87's 66-bit pi argument reduction against an "
                       "exact libm)",
}
# Cases expected to differ only on one target ("ppc-be" or "host").
EXPECTED_ON: dict[str, dict[str, str]] = {
    "ppc-be": {
        "sse_arith": "an invalid operation (inf - inf, 0 * inf) gives x86's default NaN 0xFFC00000 (sign "
                     "set) but PowerPC's 0x7FC00000. Every NaN test agrees (isnan, ucomiss, x != x); only "
                     "code reading a generated NaN's sign bit sees it. Reproducing it would cost a test "
                     "on every float operation",
    },
    "host": {},
}

GUEST_SCRATCH = 0x00100000
GUEST_STACK = 0x00300000
GUEST_SIZE = 0x00400000
REC = struct.Struct("<IIIHH8Q128s64s64s")   # in_a in_b eax sw eax_in_scratch st[8] xmm before after


def run(cmd, cwd=None, **kw):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError("$ %s\n%s%s" % (" ".join(map(str, cmd)), r.stdout[-3000:], r.stderr[-6000:]))
    return r.stdout


def gold_source(cases):
    """The x86 recorder: runs each nat_<case> on every input and writes golden.bin."""
    from tools.conformance import harness as H
    out = ["#include <stdio.h>", "#include <string.h>", "#include <stdint.h>", "#include <math.h>",
           "extern unsigned int g_in_a, g_in_b, g_out_eax; extern unsigned short g_out_sw;",
           "extern double g_out_st[8]; extern unsigned char g_out_xmm[128];",
           "extern unsigned char *g_scratch_ptr;",
           "static unsigned char g_scratch[64] __attribute__((aligned(16)));",
           "static FILE *g_f;"]
    out += [f"void nat_{c['name']}(void);" for c in cases]
    out.append("""
static void rec(unsigned in_a, unsigned in_b, const unsigned char *before, int eax_was_scratch) {
    unsigned e = g_out_eax;
    unsigned short pad = 0;
    uintptr_t s = (uintptr_t)g_scratch;
    /* eax started as the scratch pointer (fpu, sse): still pointing into it, or with only its low
       half replaced (fnstsw ax), it is recorded relative to the scratch so the replay can rebase it */
    if (eax_was_scratch && e >= s && e < s + 64) { e = (unsigned)(e - s); pad = 1; }
    else if (eax_was_scratch && (e >> 16) == (unsigned)(s >> 16)) { e &= 0xFFFFu; pad = 2; }
    fwrite(&in_a, 4, 1, g_f); fwrite(&in_b, 4, 1, g_f); fwrite(&e, 4, 1, g_f);
    fwrite(&g_out_sw, 2, 1, g_f); fwrite(&pad, 2, 1, g_f);
    fwrite(g_out_st, 8, 8, g_f); fwrite(g_out_xmm, 1, 128, g_f);
    fwrite(before, 1, 64, g_f); fwrite(g_scratch, 1, 64, g_f);
}
int main(int argc, char **argv) {
    unsigned char before[64];
    if (argc < 2 || !(g_f = fopen(argv[1], "wb"))) return 2;
    g_scratch_ptr = g_scratch;""")
    for c in cases:
        for inp in c["inputs"]:
            ld = H._load_inputs(c["kind"], inp)
            if c["kind"] == "gpr":
                ab = f"0x{inp[0]:08X}u, 0x{inp[1]:08X}u"
                ld = "memset(g_scratch, 0, sizeof g_scratch); " + ld
            else:
                ab = "0u, 0u"
            flag = "1" if c["kind"] != "gpr" else "0"
            out.append(f"    {{ g_out_eax = 0; g_out_sw = 0; memset(g_out_st, 0, sizeof g_out_st);"
                       f" memset(g_out_xmm, 0, sizeof g_out_xmm); {ld}"
                       f" memcpy(before, g_scratch, 64); nat_{c['name']}(); rec({ab}, before, {flag}); }}")
    out.append("    fclose(g_f);\n    return 0;\n}")
    return "\n".join(out) + "\n"


PRELUDE = r"""/* generated by tests/xbox_recomp/conformance.py: the lifted side, replaying x86 recordings */
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

/* the runtime ABI the lifted snippets touch */
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
volatile uint32_t g_xb_attention; void wl_xb_safe_point(void); void wl_xb_safe_point(void) {} /* RECOMP_BACKEDGE (no other threads here) */
RECOMP_TLS uint32_t g_seh_ebp, g_ebp, g_fs_base;
RECOMP_TLS int g_df;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS int g_fp_cmp; RECOMP_TLS uint16_t g_fp_cc = 0x4000;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
int g_force_return;
void recomp_icall_fail_log(uint32_t va) { (void)va; }
void recomp_icall_not_code_log(uint32_t va) { (void)va; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
void recomp_unimpl(const char *t, uint32_t va) { printf("UNIMPL %s at %08x\n", t, (unsigned)va); }
uint64_t xbox_ReadTimeStampCounter(void) { return 0; }
void recomp_debug_service(uint32_t s, uint32_t a) { (void)s; (void)a; }

#define GUEST_SCRATCH @SCRATCH@u
#define GUEST_STACK @STACK@u
#define GUEST_SIZE @SIZE@u

struct kase { const char *name; int kind; int nvec; double tol; void (*fn)(void); };

static void regs_reset(void) {
    memset(&g_xmm0, 0, sizeof(RecompXmm) ); memset(&g_xmm1, 0, sizeof(RecompXmm));
    memset(&g_xmm2, 0, sizeof(RecompXmm)); memset(&g_xmm3, 0, sizeof(RecompXmm));
    memset(&g_xmm4, 0, sizeof(RecompXmm)); memset(&g_xmm5, 0, sizeof(RecompXmm));
    memset(&g_xmm6, 0, sizeof(RecompXmm)); memset(&g_xmm7, 0, sizeof(RecompXmm));
    memset(&g_mm0, 0, sizeof(RecompMmx)); memset(&g_mm1, 0, sizeof(RecompMmx));
    memset(&g_mm2, 0, sizeof(RecompMmx)); memset(&g_mm3, 0, sizeof(RecompMmx));
    memset(&g_mm4, 0, sizeof(RecompMmx)); memset(&g_mm5, 0, sizeof(RecompMmx));
    memset(&g_mm6, 0, sizeof(RecompMmx)); memset(&g_mm7, 0, sizeof(RecompMmx));
    memset(g_fp_stack, 0, sizeof g_fp_stack);
    g_fp_top = 0; g_fp_control_word = 0x027Fu; g_fp_cmp = 0; g_fp_cc = 0x4000;
    g_df = 0; g_ebx = g_esi = g_edi = 0; g_ebp = g_seh_ebp = 0;
    g_esp = GUEST_STACK;
}
"""

FP_MACROS = r"""
#define fp_push(v) do { double _fp_value = (v); g_fp_top = (g_fp_top + 7u) & 7u; g_fp_stack[g_fp_top] = _fp_value; } while (0)
#define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
#define fp_top() g_fp_stack[g_fp_top]
#define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
#define fp_st1() fp_st(1)
"""

REPLAY = r"""
struct rec { uint32_t in_a, in_b, eax; uint16_t sw, eax_in_scratch; uint64_t st[8]; uint8_t xmm[128], before[64], after[64]; };
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int read_rec(FILE *f, struct rec *r) {
    uint8_t b[@RECSIZE@]; int i;
    if (fread(b, 1, sizeof b, f) != sizeof b) return 0;
    r->in_a = le32(b); r->in_b = le32(b + 4); r->eax = le32(b + 8);
    r->sw = (uint16_t)(b[12] | b[13] << 8);
    r->eax_in_scratch = (uint16_t)(b[14] | b[15] << 8);
    for (i = 0; i < 8; i++) r->st[i] = (uint64_t)le32(b + 16 + 8 * i) | (uint64_t)le32(b + 20 + 8 * i) << 32;
    memcpy(r->xmm, b + 80, 128); memcpy(r->before, b + 208, 64); memcpy(r->after, b + 272, 64);
    return 1;
}
static double bits_to_double(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static int same_double(double a, double b, double tol) {
    if (memcmp(&a, &b, sizeof a) == 0) return 1;
    if (isnan(a) && isnan(b)) return 1;
    if (tol > 0.0 && !isnan(a) && !isnan(b)) {
        double d = fabs(a - b), s = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
        return d <= tol * (s > 1.0 ? s : 1.0);
    }
    return 0;
}
static void hex(char *o, const uint8_t *p, int n) { int i; for (i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", p[i]); }

int main(int argc, char **argv) {
    FILE *f;
    unsigned c, v;
    int total = 0, bad_cases = 0;
    static uint8_t guest_mem[GUEST_SIZE];
    uint8_t *guest = guest_mem;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { printf("cannot open the golden file\n"); return 2; }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    for (c = 0; c < sizeof(K) / sizeof(K[0]); c++) {
        const struct kase *k = &K[c];
        int bad = 0;
        char why[1024] = "";
        for (v = 0; v < (unsigned)k->nvec; v++) {
            struct rec r;
            uint32_t want_eax, l_eax;
            uint8_t xmm[128], after[64];
            if (!read_rec(f, &r)) { printf("golden file ends early at %s\n", k->name); return 2; }
            regs_reset();
            memset(guest + GUEST_SCRATCH, 0, 64);
            memcpy(guest + GUEST_SCRATCH, r.before, 64);
            if (k->kind == 0) { g_eax = r.in_a; g_ecx = r.in_b; g_edx = 0; }
            else { g_eax = GUEST_SCRATCH; g_ecx = 0; g_edx = 0; }
            k->fn();
            total++;
            want_eax = r.eax_in_scratch == 1 ? GUEST_SCRATCH + r.eax
                     : r.eax_in_scratch == 2 ? (GUEST_SCRATCH & 0xFFFF0000u) | r.eax : r.eax;
            l_eax = g_eax;
            memcpy(xmm +   0, &g_xmm0, 16); memcpy(xmm +  16, &g_xmm1, 16);
            memcpy(xmm +  32, &g_xmm2, 16); memcpy(xmm +  48, &g_xmm3, 16);
            memcpy(xmm +  64, &g_xmm4, 16); memcpy(xmm +  80, &g_xmm5, 16);
            memcpy(xmm +  96, &g_xmm6, 16); memcpy(xmm + 112, &g_xmm7, 16);
            memcpy(after, guest + GUEST_SCRATCH, 64);
            if (!bad) {
                if (l_eax != want_eax) {
                    bad = 1; snprintf(why, sizeof why, "vec %u eax native=%08x lifted=%08x", v, want_eax, l_eax);
                } else if (k->kind == 1) {
                    int nd = (8 - ((r.sw >> 11) & 7)) & 7, ld = (8 - (g_fp_top & 7)) & 7, i;
                    if (nd != ld) { bad = 1; snprintf(why, sizeof why, "vec %u x87 depth native=%d lifted=%d", v, nd, ld); }
                    for (i = 0; !bad && i < nd; i++) {
                        double n = bits_to_double(r.st[i]), l = g_fp_stack[(g_fp_top + i) & 7];
                        if (!same_double(n, l, k->tol)) {
                            bad = 1; snprintf(why, sizeof why, "vec %u st(%d) native=%.17g lifted=%.17g", v, i, n, l);
                        }
                    }
                } else if (k->kind == 2 && memcmp(xmm, r.xmm, 128) != 0) {
                    int i; char a[33], b[33];
                    for (i = 0; i < 8 && !memcmp(xmm + 16 * i, r.xmm + 16 * i, 16); i++) {}
                    hex(a, r.xmm + 16 * i, 16); hex(b, xmm + 16 * i, 16);
                    bad = 1; snprintf(why, sizeof why, "vec %u xmm%d native=%s lifted=%s", v, i, a, b);
                }
                if (!bad && memcmp(after, r.after, 64) != 0) {
                    char a[129], b[129];
                    hex(a, r.after, 64); hex(b, after, 64);
                    bad = 1; snprintf(why, sizeof why, "vec %u scratch native=%s lifted=%s", v, a, b);
                }
            }
            if (bad) bad++;
        }
        if (bad) { bad_cases++; printf("CASE %s FAIL %d/%d %s\n", k->name, bad - 1, k->nvec, why); }
        else printf("CASE %s PASS %d\n", k->name, k->nvec);
    }
    fclose(f);
    printf("TOTAL %d vectors, %d failing cases\n", total, bad_cases);
    return 0;
}
"""


def replay_source(prepared):
    out = [PRELUDE.replace("@SCRATCH@", "0x%08X" % GUEST_SCRATCH).replace("@STACK@", "0x%08X" % GUEST_STACK)
           .replace("@SIZE@", "0x%08X" % GUEST_SIZE), FP_MACROS]
    for p in prepared:
        body = "\n".join(f"    {l}" for l in p["lines"]) or "    /* nothing */"
        out.append(f"""static void lif_{p['name']}(void) {{
    uint32_t ebp = 0; int _cf = 0; int _flags = 0;
    uint32_t _fa = 0, _fb = 0; int32_t _fas = 0, _fbs = 0; double _fca = 0.0, _fcb = 0.0;
    (void)ebp; (void)_cf; (void)_flags; (void)_fa; (void)_fb; (void)_fas; (void)_fbs; (void)_fca; (void)_fcb;
{body}
}}""")
    kinds = {"gpr": 0, "fpu": 1, "sse": 2}
    out.append("static const struct kase K[] = {")
    for p in prepared:
        out.append(f'    {{"{p["name"]}", {kinds[p["kind"]]}, {p["nvec"]}, {p["tol"]!r}, lif_{p["name"]}}},')
    out.append("};")
    return "\n".join(out) + "\n" + REPLAY.replace("@RECSIZE@", str(REC.size))


def tool(name):
    return shutil.which(name)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    ap.add_argument("--build", required=True, help="work directory (in the build tree)")
    ap.add_argument("--target", choices=("ppc-be", "host", "both"), default="both")
    ap.add_argument("--header", default=str(WL_ROOT / "wl" / "xboxrt" / "recomp"),
                    help="directory holding recomp_types.h, or 'upstream' for the patched template")
    ap.add_argument("-k", help="only cases whose name contains this")
    ap.add_argument("--opt", default="-Os", help="optimisation for the lifted side (default -Os)")
    ap.add_argument("--unpatched", action="store_true",
                    help="lift with the vendored pipeline as upstream ships it (to see what the patches fix)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every case's result")
    args = ap.parse_args(argv)
    work = Path(args.build).resolve()
    work.mkdir(parents=True, exist_ok=True)

    targets = ["ppc-be", "host"] if args.target == "both" else [args.target]
    need = ["gcc", "objdump"] + (["powerpc-linux-gnu-gcc", "qemu-ppc"] if "ppc-be" in targets else [])
    missing = [t for t in need if not tool(t)]
    if missing:
        print("skipped: missing " + ", ".join(missing))
        return SKIP
    probe = work / "m32_probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    if subprocess.run(["gcc", "-m32", str(probe), "-o", str(work / "m32_probe")], capture_output=True).returncode:
        print("skipped: gcc -m32 cannot build 32-bit x86 programs (install gcc-multilib)")
        return SKIP

    try:
        if args.unpatched:
            pipe = xbox_recomp.prepare_pipeline(work / "pipeline-unpatched", quiet=True,
                                                patches_dir=work / "no-patches")
        else:
            pipe = xbox_recomp.prepare_pipeline(work / "pipeline", quiet=True)
        sys.path.insert(0, str(pipe))
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        sys.dont_write_bytecode = True
        from tools.conformance.cases import CASES
        from tools.conformance import harness as H
        from tools.conformance.__main__ import _bytes_from_objdump, _lift
        cases = [c for c in CASES if not args.k or args.k in c["name"]]
        if not cases:
            print("no cases match")
            return 2
        (work / "native.c").write_text(H.native_source(cases, dialect="gas"))
        run(["gcc", "-m32", "-masm=intel", "-msse", "-msse2", "-fno-pic", "-O0", "-c", "native.c", "-o", "native.o"],
            cwd=work)
        dump = run(["objdump", "-d", "-M", "intel", "native.o"], cwd=work)
        prepared, unimpl = [], {}
        for c in cases:
            code = _bytes_from_objdump(dump, c["name"])
            lines, _mn = _lift(code)
            for l in lines:
                if "RECOMP_UNIMPL" in l or "TODO" in l:
                    unimpl.setdefault(c["name"], []).append(l.strip())
            prepared.append(dict(name=c["name"], kind=c["kind"], lines=lines, nvec=len(c["inputs"]),
                                 tol=float(c.get("tol", 0.0))))
        (work / "gold.c").write_text(gold_source(cases))
        run(["gcc", "-m32", "-msse2", "-fno-pic", "-no-pie", "-O0", "gold.c", "native.o", "-o", "gold_x86", "-lm"],
            cwd=work)
        run([str(work / "gold_x86"), "golden.bin"], cwd=work)
        nrec = (work / "golden.bin").stat().st_size // REC.size
        assert nrec == sum(p["nvec"] for p in prepared), (nrec, sum(p["nvec"] for p in prepared))
        (work / "replay.c").write_text(replay_source(prepared))

        if args.header == "upstream":
            inc = pipe / "templates" / "runtime"
        else:
            inc = Path(args.header)
        flags = ["-std=gnu11", args.opt, "-ffp-contract=off", "-fno-strict-aliasing", "-w", f"-I{inc}"]
        builds = {"ppc-be": (["powerpc-linux-gnu-gcc", "-static", "-mcpu=750"] + flags, ["qemu-ppc"]),
                  "host": (["gcc"] + flags, [])}
        results = {}
        for t in targets:
            cc, emu = builds[t]
            exe = work / f"replay_{t}"
            run(cc + ["replay.c", "-o", str(exe), "-lm"], cwd=work)
            out = run(emu + [str(exe), "golden.bin"], cwd=work)
            (work / f"result_{t}.txt").write_text(out)
            results[t] = out
    except RuntimeError as e:
        print(f"harness error:\n{e}")
        return 2

    rc = 0
    n_cases = len(prepared)
    for t, out in results.items():
        expected = dict(EXPECTED)
        expected.update(EXPECTED_ON.get(t, {}))
        fails, passes, total = {}, set(), "?"
        for line in out.splitlines():
            parts = line.split(" ", 3)
            if parts[0] == "CASE" and parts[2] == "PASS":
                passes.add(parts[1])
            elif parts[0] == "CASE":
                fails[parts[1]] = parts[3] if len(parts) > 3 else ""
            elif parts[0] == "TOTAL":
                total = parts[1]
            elif parts[0] == "UNIMPL" and args.verbose:
                print(f"[{t}] {line}")
        unexpected = {n: w for n, w in fails.items() if n not in expected}
        xpass = sorted(n for n in passes if n in expected)
        ok = not unexpected and not xpass and len(passes) + len(fails) == n_cases
        print(f"[{t}] {len(passes)}/{n_cases} cases pass, {total} vectors; "
              f"{len(fails) - len(unexpected)} expected failure(s), {len(unexpected)} unexpected, "
              f"{len(xpass)} unexpectedly passing: {'OK' if ok else 'FAIL'}")
        for n, w in sorted(unexpected.items()):
            print(f"[{t}]   FAIL {n}: {w}")
        for n in xpass:
            print(f"[{t}]   PASSES but listed as expected ({expected[n]}): remove it from EXPECTED")
        for n, w in sorted(fails.items()):
            if n in expected:
                print(f"[{t}]   expected: {n} ({expected[n]})")
        if args.verbose:
            print(out)
        if not ok:
            rc = 1
    if unimpl:
        print(f"note: {len(unimpl)} case(s) lifted with unimplemented instructions: {sorted(unimpl)}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
