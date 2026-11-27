/*
 * st_player.c - the Sound Tracker (compiled) AY music player used by
 * Manic Miner 128, translated from Z80 to C.
 *
 * The player lives at 49152 (init) / 49158 (play) in the game's memory and
 * plays the module at 50236 ("SONG BY ST COMPILE..."), Mihai Novitchi's
 * arrangement of "In the Hall of the Mountain King". As with the game code,
 * all of the player's state stays in mem[] at its original addresses (it
 * patches its own operands: 49339, 49980, 49988), and the AY registers are
 * written through zx_ay_write() in the same order as the OUT/OUTD loop at
 * 50207.
 *
 * The translation keeps the Z80 registers as variables because the original
 * relies on stale register contents in places (e.g. channel A's mixer bits
 * when the channel is silent use whatever C held on entry).
 *
 * SPDX-License-Identifier: MIT (player translation); module (C) its authors
 */
#include <stdint.h>
#include "zx.h"
#include "st_player.h"

#define M(a)  mem[(uint16_t)(a)]
#define T(n)  zx_tick(n)

/* Player addresses (this compile of the player). */
#define ST_MODULE        50236
#define ST_DELAY         49272   /* tempo (module byte 0) */
#define ST_DELAY_CNT     49273
#define ST_POS_COUNT     49274   /* number of positions + 1 */
#define ST_POSITIONS     49264   /* word: positions table */
#define ST_ORNAMENTS     49266   /* word: ornaments */
#define ST_PATTERNS      49268   /* word: patterns */
#define ST_SAMPLES       49270   /* word: samples */
#define ST_PAT_A         49275   /* words: pattern pointers A, B, C */
#define ST_PAT_B         49277
#define ST_PAT_C         49279
#define ST_CHAN_A        49284   /* channel blocks, 10 bytes each, IX-2..IX+7 */
#define ST_CHAN_B        49294
#define ST_CHAN_C        49304
#define ST_POSITION      49312
#define ST_AY            49313   /* AY register mirror R0..R13 */
#define ST_BASE_OPERAND  49339   /* operand of LD BC,nn at 49338 */
#define ST_POS_OPERAND   49980   /* operand of LD DE,nn at 49979 */
#define ST_TRANSPOSE     49988   /* operand of ADD A,n at 49987 */
#define ST_NOTE_TABLE    50015

static uint8_t A, B, C, D, E, H, L;
static uint16_t IX;

#define HL  ((uint16_t)((H << 8) | L))
#define DE  ((uint16_t)((D << 8) | E))
#define SET_HL(v) do { uint16_t _v = (uint16_t)(v); H = (uint8_t)(_v >> 8); L = (uint8_t)_v; } while (0)
#define SET_DE(v) do { uint16_t _v = (uint16_t)(v); D = (uint8_t)(_v >> 8); E = (uint8_t)_v; } while (0)
#define IXo(o) M(IX + (o))

/* 49327: CP (HL); RET Z; ADD HL,BC; JP 49327 */
static void find(uint16_t bc)
{
    while (M(HL) != A) { SET_HL(HL + bc); T(7 + 11 + 10); }
    T(7 + 11);
}

/* 49333: DE = word at HL + module base; HL += 2. (49338 is the second half.) */
static void read_ptr(void)
{
    E = M(HL); SET_HL(HL + 1); D = M(HL); SET_HL(HL + 1);
    uint16_t base = (uint16_t)(M(ST_BASE_OPERAND) | (M(ST_BASE_OPERAND + 1) << 8));
    SET_DE(DE + base);
    T(7 + 6 + 7 + 6 + 4 + 10 + 11 + 4 + 10);
}

/* 50207: copy the AY mirror to the chip, R13 (or R10) down to R0. */
static void ay_out(void)
{
    uint16_t hl = ST_AY + 13;
    uint8_t reg = 13;
    T(10 + 4 + 7 + 7 + 12);
    if (M(hl) == 0) { reg = 10; hl -= 3; T(-12 + 7 + 7 + 6 * 3); }
    for (;;) {
        zx_ay_write(reg, M(hl));
        hl--;
        T(7 + 7 + 12 + 7 + 16 + 4 + 10);
        if (reg == 0) break;
        reg--;
    }
    A = 0xFF;
    T(10);
}

void st_init(void)
{
    /* 49152: LD HL,50236; JP 49161 */
    SET_HL(ST_MODULE);
    T(10 + 10);
    zx_iff = 0;                                     /* DI */
    A = M(HL);
    M(ST_DELAY) = A;
    M(ST_BASE_OPERAND) = L; M(ST_BASE_OPERAND + 1) = H;
    SET_HL(HL + 1);
    T(4 + 7 + 13 + 16 + 6 + 17);
    read_ptr();
    A = M(DE); SET_DE(DE + 1); A++;
    M(ST_POS_COUNT) = A;
    M(ST_POSITIONS) = E; M(ST_POSITIONS + 1) = D;
    T(7 + 6 + 4 + 13 + 20 + 17);
    read_ptr();
    M(ST_ORNAMENTS) = E; M(ST_ORNAMENTS + 1) = D;
    uint16_t pushed = DE;
    T(20 + 11 + 17);
    read_ptr();
    M(ST_PATTERNS) = E; M(ST_PATTERNS + 1) = D;
    SET_HL(27);
    T(20 + 10 + 17);
    /* CALL 49338: DE = HL + base */
    {
        uint16_t base = (uint16_t)(M(ST_BASE_OPERAND) | (M(ST_BASE_OPERAND + 1) << 8));
        SET_DE(HL + base);
        T(10 + 11 + 4 + 10);
    }
    { uint16_t t = HL; SET_HL(DE); SET_DE(t); }    /* EX DE,HL */
    M(ST_SAMPLES) = L; M(ST_SAMPLES + 1) = H;
    M(ST_PAT_A) = 49281 & 0xFF; M(ST_PAT_A + 1) = 49281 >> 8;
    T(4 + 16 + 10 + 16);
    /* Clear 49282..49326 */
    for (uint16_t a = 49282; a <= 49326; a++) M(a) = 0;
    B = 0; C = 0;
    SET_HL(49326); SET_DE(49327);
    T(10 + 10 + 10 + 7 + 21 * 44 - 5);
    SET_HL(pushed);                                 /* POP HL */
    A = 0;
    T(10 + 10 + 4 + 17);
    find(33);
    A--;
    M(ST_CHAN_A + 7) = A; M(ST_CHAN_B + 7) = A; M(ST_CHAN_C + 7) = A;
    M(ST_DELAY_CNT) = 1;
    SET_HL(HL + 1);
    M(ST_CHAN_A + 5) = L; M(ST_CHAN_A + 6) = H;
    M(ST_CHAN_B + 5) = L; M(ST_CHAN_B + 6) = H;
    M(ST_CHAN_C + 5) = L; M(ST_CHAN_C + 6) = H;
    B = 0; C = 33;
    T(4 + 13 * 3 + 7 + 13 + 6 + 16 * 3 + 17);
    ay_out();
    zx_iff = 1;                                     /* EI */
    T(4 + 10);
}

/* 49465: DEC (IX+2); RET P; reload. Returns 1 if P (no new row yet). */
static int skip_counter(void)
{
    uint8_t v = (uint8_t)(IXo(2) - 1);
    IXo(2) = v;
    T(23 + 11);
    if (!(v & 0x80)) return 1;
    IXo(2) = IXo(-1);
    T(-6 + 19 + 19 + 10);
    return 0;
}

/* 49400: move to the next position. */
static void next_position(void)
{
    A = M(ST_POSITION);
    C = A;
    T(13 + 4 + 10 + 7 + 10);
    if (!(A < M(ST_POS_COUNT))) { A = 0; C = 0; T(4 + 4); }
    A++;
    M(ST_POSITION) = A;
    L = C; H = 0;
    SET_HL(HL * 2);
    SET_HL(HL + (uint16_t)(M(ST_POSITIONS) | (M(ST_POSITIONS + 1) << 8)));
    C = M(HL);
    SET_HL(HL + 1);
    A = M(HL);
    M(ST_TRANSPOSE) = A;
    A = C;
    SET_HL(M(ST_PATTERNS) | (M(ST_PATTERNS + 1) << 8));
    B = 0; C = 7;
    T(4 + 13 + 4 + 7 + 11 + 20 + 11 + 7 + 6 + 7 + 13 + 4 + 16 + 10 + 17);
    find(7);
    SET_HL(HL + 1);
    T(6 + 17);
    read_ptr();
    M(ST_PAT_A) = E; M(ST_PAT_A + 1) = D;
    T(20 + 17);
    read_ptr();
    M(ST_PAT_B) = E; M(ST_PAT_B + 1) = D;
    T(20 + 17);
    read_ptr();
    M(ST_PAT_C) = E; M(ST_PAT_C + 1) = D;
    T(20 + 10);
}

/* 49560: interpret pattern data for the channel at IX, from HL. */
static void parse(void)
{
    for (;;) {
        A = M(HL);
        T(7 + 7 + 10);
        if (A < 96) {
            /* Note */
            IXo(1) = A; IXo(0) = 0; IXo(7) = 32;
            SET_HL(HL + 1);
            T(19 + 19 + 19 + 6 + 10);
            return;
        }
        T(7 + 10);
        if (A < 112) {
            /* Sample */
            A -= 96;
            uint16_t saved = HL;
            B = 0; C = 99;
            SET_HL(M(ST_SAMPLES) | (M(ST_SAMPLES + 1) << 8));
            T(7 + 11 + 10 + 16 + 17);
            find(99);
            SET_HL(HL + 1);
            IXo(3) = L; IXo(4) = H;
            SET_HL(saved + 1);
            T(6 + 19 + 19 + 10 + 6 + 10);
            continue;
        }
        T(7 + 10);
        if (A < 128 || A == 130) {
            /* Ornament (130 = ornament 0) */
            if (A == 130) A = 0; else A -= 112;
            uint16_t saved = HL;
            B = 0; C = 33;
            SET_HL(M(ST_ORNAMENTS) | (M(ST_ORNAMENTS + 1) << 8));
            T(7 + 11 + 10 + 16 + 17);
            find(33);
            SET_HL(HL + 1);
            IXo(5) = L; IXo(6) = H;
            IXo(-2) = 0;
            SET_HL(saved + 1);
            T(6 + 19 + 19 + 19 + 10 + 6 + 10);
            continue;
        }
        if (A == 128) {
            /* Rest: sample off */
            SET_HL(HL + 1);
            IXo(7) = 255;
            T(10 + 6 + 19 + 10);
            return;
        }
        T(10 + 7 + 10);
        if (A == 129) {
            /* Empty */
            SET_HL(HL + 1);
            T(6 + 10);
            return;
        }
        T(7 + 10 + 7 + 10);
        if (A < 143) {
            /* Envelope: shape, period */
            A -= 128;
            M(ST_AY + 13) = A;
            SET_HL(HL + 1);
            A = M(HL);
            SET_HL(HL + 1);
            M(ST_AY + 11) = A;
            IXo(-2) = 1;
            uint16_t saved = HL;
            A = 0;
            B = 0; C = 33;
            SET_HL(M(ST_ORNAMENTS) | (M(ST_ORNAMENTS + 1) << 8));
            T(7 + 13 + 6 + 7 + 6 + 13 + 19 + 11 + 4 + 10 + 16 + 17);
            find(33);
            SET_HL(HL + 1);
            IXo(5) = L; IXo(6) = H;
            SET_HL(saved);
            T(6 + 19 + 19 + 10 + 10);
            continue;
        }
        /* Skip count */
        A = (uint8_t)(A - 161);
        IXo(2) = A; IXo(-1) = A;
        SET_HL(HL + 1);
        T(7 + 19 + 19 + 6 + 10);
    }
}

/* 49717: advance the sample position of the channel at IX. */
static void sample_tick(void)
{
    A = (uint8_t)(IXo(7) + 1);                      /* LD A,(IX+7); INC A */
    T(19 + 4 + 5);
    if (A == 0) { T(6); return; }                   /* RET Z */
    A = (uint8_t)(A - 1);                           /* DEC A */
    A = (uint8_t)(A - 1);                           /* DEC A */
    IXo(7) = A;
    int zero = (A == 0);
    A = IXo(0);
    C = A;
    A = (uint8_t)((A + 1) & 31);
    IXo(0) = A;
    T(4 + 4 + 19 + 11 + 19 + 4 + 4 + 7 + 19 + 10 + 5);
    if (!zero) { T(6); return; }
    E = IXo(3); D = IXo(4);
    SET_HL(96 + DE);
    A = (uint8_t)(M(HL) - 1);
    T(19 + 19 + 10 + 11 + 7 + 4 + 10);
    if (A & 0x80) { IXo(7) = 255; T(19 + 10); return; }
    C = A;
    A = (uint8_t)((A + 1) & 31);
    IXo(0) = A;
    SET_HL(HL + 1);
    A = (uint8_t)(M(HL) + 1);
    IXo(7) = A;
    T(4 + 4 + 7 + 19 + 6 + 7 + 4 + 19 + 10);
}

/* 49344: decode sample line A (position) at IX (sample data). */
static void sample_line(void)
{
    D = 0; E = A;
    A = (uint8_t)(A + A);
    A = (uint8_t)(A + E);
    E = A;
    IX = (uint16_t)(IX + DE);
    A = IXo(1);
    C = (A & 0x80) ? 16 : D;
    B = (A & 0x40) ? 2 : D;
    A &= 31;
    H = A;
    E = IXo(2);
    A = IXo(0);
    D = (uint8_t)((A & 0xF0) >> 4);
    L = A & 15;
    T(7 + 4 + 4 + 4 + 4 + 15 + 19 + 8 + 7 + 10 + 4 + 8 + 7 + 10 + 4 + 7 + 4 + 19 + 19 + 11 + 7 + 16 + 4 + 10 + 7 + 4 + 20);
    if (IXo(1) & 0x20) { D |= 0x10; T(8 + 10 - 5); }
    else T(6);
}

/* 49769: noise period from the sample if its noise is enabled. */
static void noise(void)
{
    A = C;
    T(4 + 4 + 5);
    if (A) { T(6); return; }
    A = H;
    M(ST_AY + 6) = A;
    T(4 + 13 + 10);
}

/* 49970: tone period for the channel at IX: note + ornament + transpose,
 * plus the sample's signed offset in DE (sign in bit 4 of D). */
static void tone(void)
{
    uint8_t vol = L;
    uint16_t offs = DE;
    SET_HL(IXo(5) | (IXo(6) << 8));
    SET_DE(M(ST_POS_OPERAND) | (M(ST_POS_OPERAND + 1) << 8));
    SET_HL(HL + DE);
    A = IXo(1);
    A = (uint8_t)(A + M(HL));
    A = (uint8_t)(A + M(ST_TRANSPOSE));
    A = (uint8_t)(A + A);
    E = A; D = 0;
    SET_HL(ST_NOTE_TABLE + DE);
    uint16_t period = (uint16_t)(M(HL) | (M(HL + 1) << 8));
    SET_HL(period);
    SET_DE(offs);
    A = vol;
    T(4 + 11 + 11 + 19 + 19 + 10 + 11 + 19 + 7 + 7 + 4 + 4 + 7 + 10 + 11 + 7 + 6 + 7 + 4 + 10 + 10 + 8);
    if (D & 0x10) {
        D &= (uint8_t)~0x10;
        SET_HL(HL + DE);
        T(7 + 8 + 11 + 10);
    } else {
        SET_HL(HL - DE);
        T(12 + 4 + 15 + 10);
    }
}

/* 49777: hardware envelope on this channel. HL = its volume register. */
static void envelope(uint16_t vol_reg)
{
    A = IXo(7);
    T(19 + 4 + 5);
    if ((uint8_t)(A + 1) == 0) { T(6); return; }
    A = IXo(-2);
    T(19 + 4 + 5);
    if (A == 0) { T(6); return; }
    T(7 + 10);
    if (A == 2) {
        A = 0;
        M(ST_AY + 13) = 0;
        T(4 + 13);
    } else {
        IXo(-2) = 2;
        T(19 + 10);
    }
    M(vol_reg) |= 0x10;
    T(15 + 10);
}

void st_play(void)
{
    /* 49476 */
    A = (uint8_t)(M(ST_DELAY_CNT) - 1);
    M(ST_DELAY_CNT) = A;
    T(13 + 4 + 13 + 10);
    if (A == 0) {
        M(ST_DELAY_CNT) = M(ST_DELAY);
        IX = ST_CHAN_A;
        T(13 + 13 + 14 + 17);
        if (!skip_counter()) {
            SET_HL(M(ST_PAT_A) | (M(ST_PAT_A + 1) << 8));
            A = (uint8_t)(M(HL) + 1);
            T(10 + 16 + 7 + 4 + 10);
            if (A == 0) { T(7); next_position(); }
            SET_HL(M(ST_PAT_A) | (M(ST_PAT_A + 1) << 8));
            T(16 + 17);
            parse();
            M(ST_PAT_A) = L; M(ST_PAT_A + 1) = H;
            T(16);
        } else T(10);
        IX = ST_CHAN_B;
        T(14 + 17);
        if (!skip_counter()) {
            SET_HL(M(ST_PAT_B) | (M(ST_PAT_B + 1) << 8));
            T(10 + 16 + 17);
            parse();
            M(ST_PAT_B) = L; M(ST_PAT_B + 1) = H;
            T(16);
        } else T(10);
        IX = ST_CHAN_C;
        T(14 + 17);
        if (!skip_counter()) {
            SET_HL(M(ST_PAT_C) | (M(ST_PAT_C + 1) << 8));
            T(10 + 16 + 17);
            parse();
            M(ST_PAT_C) = L; M(ST_PAT_C + 1) = H;
            T(16 + 10);
        } else T(10);
    }

    /* 49806: channel A */
    IX = ST_CHAN_A;
    T(14 + 17);
    sample_tick();
    A = C;
    M(ST_POS_OPERAND) = A;
    IX = (uint16_t)(M(ST_CHAN_A + 3) | (M(ST_CHAN_A + 4) << 8));
    T(4 + 13 + 20 + 17);
    sample_line();
    A = (uint8_t)(C | B);
    A = (uint8_t)((A >> 1) | (A << 7));               /* RRCA */
    M(ST_AY + 7) = A;
    IX = ST_CHAN_A;
    A = (uint8_t)(IXo(7) + 1);
    T(4 + 4 + 4 + 13 + 14 + 19 + 4 + 10);
    if (A != 0) {
        T(17);
        noise();
        T(17);
        tone();
        M(ST_AY + 0) = L; M(ST_AY + 1) = H;
        T(16);
    }
    M(ST_AY + 8) = A;
    T(10 + 7 + 17);
    envelope(ST_AY + 8);

    /* Channel B */
    IX = ST_CHAN_B;
    T(14 + 17);
    sample_tick();
    A = (uint8_t)(IXo(7) + 1);
    T(19 + 4 + 10);
    if (A != 0) {
        A = C;
        M(ST_POS_OPERAND) = A;
        IX = (uint16_t)(M(ST_CHAN_B + 3) | (M(ST_CHAN_B + 4) << 8));
        T(4 + 13 + 20 + 17);
        sample_line();
        A = (uint8_t)(M(ST_AY + 7) | C | B);
        M(ST_AY + 7) = A;
        T(13 + 4 + 4 + 13 + 17);
        noise();
        IX = ST_CHAN_B;
        T(14 + 17);
        tone();
        M(ST_AY + 2) = L; M(ST_AY + 3) = H;
        T(16);
    }
    M(ST_AY + 9) = A;
    T(10 + 7 + 17);
    envelope(ST_AY + 9);

    /* Channel C */
    IX = ST_CHAN_C;
    T(14 + 17);
    sample_tick();
    A = (uint8_t)(IXo(7) + 1);
    T(19 + 4 + 10);
    if (A != 0) {
        A = C;
        M(ST_POS_OPERAND) = A;
        IX = (uint16_t)(M(ST_CHAN_C + 3) | (M(ST_CHAN_C + 4) << 8));
        T(4 + 13 + 20 + 17);
        sample_line();
        A = M(ST_AY + 7);
        C = (uint8_t)((C << 1) | (C >> 7));
        B = (uint8_t)((B << 1) | (B >> 7));
        A = (uint8_t)(A | B | C);
        M(ST_AY + 7) = A;
        T(13 + 8 + 8 + 4 + 4 + 13 + 17);
        noise();
        IX = ST_CHAN_C;
        T(14 + 17);
        tone();
        M(ST_AY + 4) = L; M(ST_AY + 5) = H;
        T(16);
    }
    M(ST_AY + 10) = A;
    T(10 + 7 + 17);
    envelope(ST_AY + 10);
    T(10);
    ay_out();
}

void st_set_entry_c(uint8_t c) { C = c; }
