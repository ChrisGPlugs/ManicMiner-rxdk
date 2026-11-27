/*
 * mm128_load.c - build Manic Miner 128's memory image from its TAP file.
 *
 * The tape ("Manic Miner 128 by Mihai Novitchi and Bob Fossil - Software
 * Projects version") is a BASIC loader plus two code blocks:
 *   t:mm1     256 bytes, loaded by BASIC at 22784 (attribute file)
 *   manic.c   15212 bytes at 30000, started with RANDOMIZE USR 30000
 * The code at 30000 unpacks the game backwards (an Elias-gamma LZ unpacker,
 * last byte at 45211 -> last byte at 65535), copies a 15-byte stub to 40016,
 * patches two jumps to use it, and jumps to 33792 (AppEntry).
 *
 * This file does the same thing in C: the unpacker below is a direct
 * translation of the Z80 routine at 30034, flags and all, so the image it
 * produces is byte-for-byte what the Spectrum would have in memory when the
 * game starts.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "mm128_load.h"

/* ---- Z80 unpacker at 30034, translated ---------------------------------- */
typedef struct {
    uint8_t *m;
    uint8_t a, b, c;
    uint16_t hl, de;
    int cf, zf;
} unpk;

static inline void add_a_a(unpk *u) { u->cf = u->a >> 7; u->a = (uint8_t)(u->a << 1); u->zf = (u->a == 0); }
static inline void rl_c(unpk *u)    { int n = u->c >> 7; u->c = (uint8_t)((u->c << 1) | u->cf); u->cf = n; u->zf = (u->c == 0); }
static inline void rl_b(unpk *u)    { int n = u->b >> 7; u->b = (uint8_t)((u->b << 1) | u->cf); u->cf = n; u->zf = (u->b == 0); }
static inline void rla(unpk *u)     { int n = u->a >> 7; u->a = (uint8_t)((u->a << 1) | u->cf); u->cf = n; }
static inline void ld_a_hl_dec(unpk *u) { u->a = u->m[u->hl]; u->hl--; }

/* LDDR from src; returns the final source pointer (HL after LDDR). */
static uint16_t lddr(unpk *u, uint16_t src)
{
    uint32_t n = (uint32_t)((u->b << 8) | u->c);
    if (!n) n = 65536;
    while (n--) { u->m[u->de] = u->m[src]; u->de--; src--; }
    u->b = u->c = 0;
    return src;
}

/* Elias-gamma bit reader: entry at 30102 (from_102) or 30107. */
static void elias(unpk *u, int from_102)
{
    if (from_102) goto L30102;
L30107:
    if (!u->zf) goto L30102;
    ld_a_hl_dec(u); rla(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); add_a_a(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); add_a_a(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); add_a_a(u); if (!u->cf) return;
L30129:
    add_a_a(u); rl_c(u); rl_b(u); add_a_a(u); ld_a_hl_dec(u); rla(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); rl_b(u); add_a_a(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); rl_b(u); add_a_a(u); if (!u->cf) return;
    add_a_a(u); rl_c(u); rl_b(u); add_a_a(u); if (u->cf) goto L30129;
    return;
L30102:
    add_a_a(u); rl_c(u); add_a_a(u); if (!u->cf) return;
    goto L30107;
}

static void unpack_30034(uint8_t *m, uint16_t hl, uint16_t de)
{
    unpk u = { m, 0, 0, 0, hl, de, 0, 0 };
    uint16_t offset;

    u.b = 0; u.c = 1;                 /* LD BC,1 */
    offset = 1;                       /* LD (30075),BC */
    m[30075] = 1; m[30076] = 0;
    u.c--;                            /* DEC C */
    u.a = 0x80;
    goto L30084;

L30046:
    u.c++; u.zf = (u.c == 0);
    add_a_a(&u);
    if (u.cf) elias(&u, 0);
    u.b--; u.zf = (u.b == 0);
    if (u.zf) return;
    u.c--;
    u.b = u.c;
    u.c = m[u.hl]; u.hl--;
    { int n = u.b & 1; u.b >>= 1; u.cf = n; }                       /* SRL B */
    { int n = u.c & 1; u.c = (uint8_t)((u.c >> 1) | (u.cf << 7)); u.cf = n; }  /* RR C */
    { uint16_t bc = (uint16_t)(((u.b << 8) | u.c) + 1); u.b = (uint8_t)(bc >> 8); u.c = (uint8_t)bc; offset = bc; }
    m[30075] = (uint8_t)offset; m[30076] = (uint8_t)(offset >> 8);   /* LD (30075),BC */
    u.b = 0; u.c = 1;
    if (u.cf) elias(&u, 1);
    { uint16_t bc = (uint16_t)(((u.b << 8) | u.c) + 1); u.b = (uint8_t)(bc >> 8); u.c = (uint8_t)bc; }
L30073:
    lddr(&u, (uint16_t)(offset + u.de));    /* PUSH HL / ... / POP HL: HL kept */
    add_a_a(&u);
    if (u.cf) goto L30046;
L30084:
    u.c++;
    add_a_a(&u);
    if (u.cf) elias(&u, 0);
    u.hl = lddr(&u, u.hl);            /* literals */
    add_a_a(&u);
    if (u.cf) goto L30046;
    u.c++;
    add_a_a(&u);
    if (u.cf) elias(&u, 0);
    goto L30073;
}

/* ---- TAP parsing --------------------------------------------------------- */
int mm128_build_image(const uint8_t *tap, long tap_len, uint8_t *image)
{
    const uint8_t *code = NULL, *screen = NULL;
    long code_len = 0;
    int pending = -1;               /* type of data block expected next */

    for (long i = 0; i + 2 <= tap_len; ) {
        long len = tap[i] | (tap[i + 1] << 8);
        const uint8_t *blk = tap + i + 2;
        if (i + 2 + len > tap_len || len < 2) return MM128_ERR_TAP;
        if (blk[0] == 0x00 && len == 19) {
            uint16_t start = (uint16_t)(blk[14] | (blk[15] << 8));
            uint16_t dlen = (uint16_t)(blk[12] | (blk[13] << 8));
            pending = -1;
            if (blk[1] == 3 && start == 30000) pending = 1;
            else if (blk[1] == 3 && dlen == 256) pending = 2;
        } else if (blk[0] == 0xFF) {
            if (pending == 1) { code = blk + 1; code_len = len - 2; }
            else if (pending == 2) { screen = blk + 1; }
            pending = -1;
        }
        i += 2 + len;
    }
    if (!code) return MM128_ERR_TAP;

    /* The exact tape this port was made against: the code block starts with
     * LD HL,45211 / LD DE,65535 / CALL 30034 and is 15212 bytes long. */
    static const uint8_t sig[] = { 0x21, 0x9B, 0xB0, 0x11, 0xFF, 0xFF, 0xCD, 0x52, 0x75 };
    if (code_len != 15212 || memcmp(code, sig, sizeof sig) != 0) return MM128_ERR_VERSION;

    memset(image, 0, 65536);
    if (screen) memcpy(image + 22784, screen, 256);
    memcpy(image + 30000, code, (size_t)code_len);

    /* 30000: LD HL,45211; LD DE,65535; CALL 30034 */
    unpack_30034(image, 45211, 65535);
    /* LD HL,30162; LD DE,40016; LD (38328),DE; LD BC,15; LDIR */
    memmove(image + 40016, image + 30162, 15);
    image[38328] = 40016 & 0xFF; image[38329] = 40016 >> 8;
    /* LD DE,40023; LD (34360),DE */
    image[34360] = 40023 & 0xFF; image[34361] = 40023 >> 8;
    /* JP 33792 */

    if (image[33792] != 0xF3) return MM128_ERR_VERSION;
    return MM128_OK;
}
