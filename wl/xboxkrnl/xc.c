// SPDX-License-Identifier: MIT
// Xc exports: SHA-1 (FIPS 180-4), RC4 and HMAC-SHA1 (RFC 2104), which titles sign their saves
// with, written here with their state in the guest's context buffers (little-endian fields). The
// public-key and block-cipher services (XcModExp, XcDESKeyParity, XcKeyTable, XcBlockCrypt*) are
// logged stubs: Xbox Live and the console's key checks have nothing to talk to.
#include "wl/xboxkrnl/kernel_internal.h"

typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
} Sha1;

static uint32_t rol(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0;
}

static void sha1_update(Sha1 *s, const uint8_t *p, uint32_t n) {
    while (n > 0) {
        uint32_t at = (uint32_t)(s->bytes & 63u);
        uint32_t take = 64u - at < n ? 64u - at : n;
        memcpy(s->block + at, p, take);
        s->bytes += take;
        p += take;
        n -= take;
        if (at + take == 64u) {
            sha1_block(s, s->block);
        }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->bytes * 8u;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while ((s->bytes & 63u) != 56u) {
        sha1_update(s, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

// The context in guest memory: h[5], bytes (8), block (64) = 92 bytes, little-endian fields.
#define SHA_CTX_BYTES 92u

static bool sha_load(uint32_t va, Sha1 *s) {
    if (!wl_xb_range_ok(va, SHA_CTX_BYTES)) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        s->h[i] = wl_xb_rd32(va + 4u * (uint32_t)i);
    }
    s->bytes = wl_xb_rd64(va + 20u);
    wl_xb_copy_out(s->block, va + 28u, 64);
    return true;
}

static void sha_store(uint32_t va, const Sha1 *s) {
    for (int i = 0; i < 5; i++) {
        wl_xb_wr32(va + 4u * (uint32_t)i, s->h[i]);
    }
    wl_xb_wr64(va + 20u, s->bytes);
    wl_xb_copy_in(va + 28u, s->block, 64);
}

static void sha_guest_update(Sha1 *s, uint32_t va, uint32_t n) {
    uint8_t buf[256];
    while (n > 0) {
        uint32_t take = n < sizeof buf ? n : (uint32_t)sizeof buf;
        wl_xb_copy_out(buf, va, take);
        sha1_update(s, buf, take);
        va += take;
        n -= take;
    }
}

// VOID XcSHAInit(PUCHAR pbSHAContext)
void k_XcSHAInit(void) {
    Sha1 s;
    sha1_init(&s);
    memset(s.block, 0, sizeof s.block);
    if (wl_xb_range_ok(ARG(0), SHA_CTX_BYTES)) {
        sha_store(ARG(0), &s);
    }
}

// VOID XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength)
void k_XcSHAUpdate(void) {
    Sha1 s;
    if (sha_load(ARG(0), &s) && wl_xb_range_ok(ARG(1), ARG(2))) {
        sha_guest_update(&s, ARG(1), ARG(2));
        sha_store(ARG(0), &s);
    }
}

// VOID XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest)
void k_XcSHAFinal(void) {
    Sha1 s;
    uint8_t out[20];
    if (sha_load(ARG(0), &s) && wl_xb_range_ok(ARG(1), 20)) {
        sha1_final(&s, out);
        wl_xb_copy_in(ARG(1), out, 20);
    }
}

// VOID XcHMAC(PBYTE pbKey, DWORD dwKeyLength, PBYTE pbInput, DWORD dwInputLength,
//             PBYTE pbInput2, DWORD dwInputLength2, PBYTE pbDigest)
void k_XcHMAC(void) {
    uint32_t key = ARG(0), klen = ARG(1), in1 = ARG(2), n1 = ARG(3), in2 = ARG(4), n2 = ARG(5), out = ARG(6);
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20], digest[20];
    if (!wl_xb_range_ok(out, 20)) {
        return;
    }
    Sha1 s;
    if (klen > 64u) {
        sha1_init(&s);
        if (wl_xb_range_ok(key, klen)) {
            sha_guest_update(&s, key, klen);
        }
        sha1_final(&s, k);
    } else if (klen > 0 && wl_xb_range_ok(key, klen)) {
        wl_xb_copy_out(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5Cu);
    }
    sha1_init(&s);
    sha1_update(&s, ipad, 64);
    if (in1 != 0 && wl_xb_range_ok(in1, n1)) {
        sha_guest_update(&s, in1, n1);
    }
    if (in2 != 0 && wl_xb_range_ok(in2, n2)) {
        sha_guest_update(&s, in2, n2);
    }
    sha1_final(&s, inner);
    sha1_init(&s);
    sha1_update(&s, opad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, digest);
    wl_xb_copy_in(out, digest, 20);
}

// RC4: the state in guest memory as S[256], i, j (258 bytes)
// VOID XcRC4Key(PUCHAR pbKeyStruct, DWORD dwKeyLength, PUCHAR pbKey)
void k_XcRC4Key(void) {
    uint32_t st = ARG(0), klen = ARG(1), key = ARG(2);
    uint8_t s[256], k[256];
    if (!wl_xb_range_ok(st, 258) || klen == 0 || klen > 256u || !wl_xb_range_ok(key, klen)) {
        return;
    }
    wl_xb_copy_out(k, key, klen);
    for (int i = 0; i < 256; i++) {
        s[i] = (uint8_t)i;
    }
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + s[i] + k[(uint32_t)i % klen]);
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    wl_xb_copy_in(st, s, 256);
    wl_xb_wr8(st + 256u, 0);
    wl_xb_wr8(st + 257u, 0);
}

// VOID XcRC4Crypt(PUCHAR pbKeyStruct, DWORD dwInputLength, PUCHAR pbInput)
void k_XcRC4Crypt(void) {
    uint32_t st = ARG(0), n = ARG(1), data = ARG(2);
    uint8_t s[256];
    if (!wl_xb_range_ok(st, 258) || !wl_xb_range_ok(data, n)) {
        return;
    }
    wl_xb_copy_out(s, st, 256);
    uint8_t i = wl_xb_rd8(st + 256u), j = wl_xb_rd8(st + 257u);
    for (uint32_t k = 0; k < n; k++) {
        i = (uint8_t)(i + 1u);
        j = (uint8_t)(j + s[i]);
        uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
        wl_xb_wr8(data + k, (uint8_t)(wl_xb_rd8(data + k) ^ s[(uint8_t)(s[i] + s[j])]));
    }
    wl_xb_copy_in(st, s, 256);
    wl_xb_wr8(st + 256u, i);
    wl_xb_wr8(st + 257u, j);
}

// The public-key and DES services: nothing to sign or decrypt with here.
void k_XcModExp(void) {
    xk_stub("XcModExp");
    RET(0);
}

void k_XcDESKeyParity(void) {
    // odd parity in each key byte, as DES wants (cheap to do for real)
    uint32_t key = ARG(0), n = ARG(1);
    if (wl_xb_range_ok(key, n)) {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = (uint8_t)(wl_xb_rd8(key + i) & 0xFEu);
            uint8_t ones = 0;
            for (int k = 1; k < 8; k++) {
                ones = (uint8_t)(ones + ((b >> k) & 1u));
            }
            wl_xb_wr8(key + i, (uint8_t)(b | ((ones & 1u) ? 0u : 1u)));
        }
    }
}

void k_XcKeyTable(void) {
    xk_stub("XcKeyTable");
}

void k_XcBlockCrypt(void) {
    xk_stub("XcBlockCrypt");
}

void k_XcBlockCryptCBC(void) {
    xk_stub("XcBlockCryptCBC");
}

void k_XcPKGetKeyLen(void) {
    xk_stub("XcPKGetKeyLen");
    RET(0);
}

void k_XcVerifyPKCS1Signature(void) {
    xk_stub("XcVerifyPKCS1Signature");
    RET(1); // "valid": there is no key to check against
}

void xk_link_xc(void) {
}
