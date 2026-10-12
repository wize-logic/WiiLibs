/* SPDX-License-Identifier: MIT */
/* What a freestanding gcc -m32 build may call: the test images are linked without a C library. */
typedef unsigned int size_t;
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}
