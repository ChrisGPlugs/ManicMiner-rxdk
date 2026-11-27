/*
 * mm_game.c - Manic Miner, translated from Z80 to C.
 *
 * This is a routine-by-routine translation of the annotated disassembly in
 * WHumphreys/Manic-Miner-Source-Code (ManicMiner.asm). Labels and comments
 * follow that file so the two can be read side by side.
 *
 * The original memory map is kept: all game state lives in mem[] at the
 * original addresses (see mm_mem.h), the screen is drawn into the real
 * display file at 0x4000, and keyboard/joystick/beeper go through
 * zx_in()/zx_out(). The Z80 T-state cost of each piece of code is charged to
 * the clock with T(), so the game runs at the original speed and the beeper
 * produces the original pitches.
 *
 * The few places where the original manipulates the stack directly
 * (KillWilly popping return addresses, DrawThePortal dropping its return
 * address before jumping to MoveToTheNextCavern, MoveDrawSkyLabs being
 * JP'd into from the main loop) are expressed as return codes that the main
 * loop turns into the same jumps.
 *
 * Manic Miner (C) 1983 Matthew Smith. This translation: SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "zx.h"
#include "mm_mem.h"
#include "mm_game.h"
#ifdef MM128
#include "st_player.h"
#endif

/* Every data access goes through zx_mem(), which applies ULA contention for
 * 0x4000-0x7FFF (where all of Manic Miner's buffers live). Instruction
 * fetches and the stack are in uncontended memory (0x8000 up). */
#define M(a)  (*zx_mem((uint16_t)(a)))
#define T(n)  zx_tick(n)

#ifndef MM128
/* Bug-Byte: cheat 6031769 on keys 1-5 / 6-0, teleport with '6' held. */
#define CHEAT_DONE    7
#define CHEAT_ROW1    0xF7FE
#define CHEAT_ROW2    0xEFFE
#define TELEPORT_BIT  0x10
#define ENTER_PORT    0xBFFE
#else
/* Software Projects: cheat TYPEWRITER on Q-T / P-Y, teleport with '9';
 * ENTER or '0' starts the game. */
#define CHEAT_DONE    10
#define CHEAT_ROW1    0xFBFE
#define CHEAT_ROW2    0xDFFE
#define TELEPORT_BIT  0x02
#define ENTER_PORT    0xAFFE
#endif


#ifdef MM128
static void UnpackData(uint16_t hl, uint16_t de, uint16_t bc, uint8_t a);
static void CheatMenu(void);
#endif

static inline uint16_t rd16(uint16_t a) { return (uint16_t)(M(a) | (M(a + 1) << 8)); }
static inline void wr16(uint16_t a, uint16_t v) { M(a) = (uint8_t)v; M(a + 1) = (uint8_t)(v >> 8); }
static inline uint8_t rotl(uint8_t v, int n) { return (uint8_t)((v << n) | (v >> (8 - n))); }
static inline uint8_t rotr(uint8_t v, int n) { return (uint8_t)((v >> n) | (v << (8 - n))); }

/* Port I/O with the Z80 cost of the IN/OUT instruction (the I/O cycle,
 * contended for the ULA's ports, comes after the opcode fetches). */
static inline uint8_t in_port(uint16_t port)
{
    T(8);
    zx_contend_io(port);
    uint8_t v = zx_in(port);
    T(4);
    return v;
}
static inline void out254(uint8_t a)
{
    T(7);
    zx_contend_io(0x00FE);
    zx_out(0x00FE, a);
    T(4);
}

/* LDIR: byte-by-byte forward copy (so overlapping fills work as on the Z80).
 * BC=0 means 65536, as on the real CPU. */
static void ldir(uint16_t hl, uint16_t de, uint16_t bc)
{
    uint32_t n = bc ? bc : 65536u;
    if (!zx_contention) {
        for (uint32_t i = 0; i < n; i++, hl++, de++)
            mem[de] = mem[hl];
        T(21 * n - 5);
        return;
    }
    /* Per byte: pc:4, pc+1:4, hl:3, de:3, de:1 x2, and when repeating
     * de:1 x5 (the PC is in uncontended memory). */
    for (uint32_t i = 0; i < n; i++, hl++, de++) {
        T(8);
        zx_contend(hl); T(3);
        uint8_t v = mem[hl];
        zx_contend(de); T(3);
        mem[de] = v;
        zx_contend(de); T(1);
        zx_contend(de); T(1);
        if (i + 1 < n)
            for (int k = 0; k < 5; k++) { zx_contend(de); T(1); }
    }
}

/* Fill using the LD (HL),A / LDIR idiom: dst[0]=v then copy forward. */
static void fill(uint16_t hl, uint8_t v, uint16_t count_after_first)
{
    M(hl) = v;
    T(7);
    ldir(hl, (uint16_t)(hl + 1), count_after_first);
}

/* DJNZ-to-self delay with B=b (b=0 -> 256 iterations). */
static inline uint32_t djnz_cost(uint8_t b) { return (b ? b : 256u) * 13u - 5u; }

/* ------------------------------------------------------------------------ */
/* DrawASprite                                                              */
/* C=0 overwrite, C=1 blend; returns non-zero (NZ) on collision in blend    */
/* mode, leaving the sprite partially drawn exactly like the original.      */
/* ------------------------------------------------------------------------ */
static int DrawASprite(uint16_t hl, uint16_t de, int blend)
{
    T(7);
    for (int b = 16; b > 0; b--) {
        for (int half = 0; half < 2; half++) {
            uint8_t a = M(de);
            if (blend) {
                if (a & M(hl)) { T(8 + 7 + 7 + 7 + 11); return 1; }
                a |= M(hl);
                T(55 - 34);
            }
            M(hl) = a;
            if (half == 0) hl = (uint16_t)((hl & 0xFF00) | (uint8_t)(hl + 1));   /* INC L */
            de++;
        }
        /* DEC L; INC H */
        hl = (uint16_t)((hl & 0xFF00) | (uint8_t)((hl & 0xFF) - 1));
        hl = (uint16_t)(hl + 0x100);
        T(128);
        uint8_t h = (uint8_t)(hl >> 8), l = (uint8_t)hl;
        if (!(h & 7)) {
            h = (uint8_t)(h - 8);
            l = (uint8_t)(l + 0x20);
            T(4 + 7 + 4 + 4 + 7 + 4 + 7 + 7 - 5);
            if (!(l & 0xE0)) { h = (uint8_t)(h + 8); T(4 + 7 + 4 + 5); }
            hl = (uint16_t)((h << 8) | l);
        }
    }
    T(4 + 10);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* PrintAMessage / PrintASingleCharacter / DrawItem                         */
/* ------------------------------------------------------------------------ */
static void DrawItem(uint16_t hl, uint16_t de, int b)
{
    while (b--) {
        M(de) = M(hl);
        hl++;
        de = (uint16_t)(de + 0x100);            /* INC D */
        T(7 + 7 + 6 + 4 + 13);
    }
    T(10);
}

static void PrintASingleCharacter(uint8_t a, uint16_t de)
{
    uint16_t hl = (uint16_t)(((0x07 << 8) | (a | 0x80)) << 3);
    T(7 + 4 + 8 + 11 * 3 + 7 + 17);
    DrawItem(hl, de, 8);
}

static void PrintAMessage(uint16_t ix, uint16_t de, uint8_t c)
{
    do {
        PrintASingleCharacter(M(ix), de);
        ix++;
        de = (uint16_t)((de & 0xFF00) | (uint8_t)(de + 1));   /* INC E (D restored) */
        T(19 + 10 + 4 + 4 + 7 + 4 + 4 + 12);
    } while (--c);
    T(10);
}

/* ------------------------------------------------------------------------ */
/* DecreaseAirRemaining - returns 1 (zero flag set) if the air has run out. */
/* ------------------------------------------------------------------------ */
static int DecreaseAirRemaining(void)
{
    uint8_t a = (uint8_t)(M(GameClock) - 4);
    M(GameClock) = a;
    T(13 + 7 + 13 + 7 + 12);
    if (a == 0xFC) {
        T(13 + 7);
        if (M(RemainingAirSupply) == 0x24) { T(11); return 1; }
        M(RemainingAirSupply)--;
        a = M(GameClock);
        T(5 + 4 + 13 + 13);
    }
    a = (uint8_t)((a & 0xE0) >> 5);            /* AND $E0; RLCA x3 */
    uint8_t e = 0;
    T(7 + 12 + 7 + 4 + 12);
    for (uint8_t b = a; b; b--) {
        e = (uint8_t)(rotr(e, 1) | 0x80);       /* RRC E; SET 7,E */
        T(8 + 8 + 13);
    }
    uint16_t hl = (uint16_t)(0x5200 | M(RemainingAirSupply));
    T(13 + 4 + 7 + 7);
    for (int b = 0; b < 4; b++) {
        M(hl) = e;
        hl = (uint16_t)(hl + 0x100);
        T(7 + 4 + 13);
    }
    T(4 + 4 + 10);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* DrawCurrentCavernToScreenBuffer                                          */
/* ------------------------------------------------------------------------ */
/* DCCTSB4_OPERAND (mm_mem.h) is the operand of the self-modified 'LD D,n'
 * at DrawCurrentCavernToScreenBuffer4 + 1; kept in mem[] for fidelity. */

static void DrawCurrentCavernToScreenBuffer2(uint16_t ix)
{
    for (int c = 0; c < 256; c++) {
        uint8_t a = M(ix);
        /* CPIR over the 72 bytes of tile definitions at BackgroundTile. */
        uint16_t hl = BackgroundTile;
        int n = 72;
        uint32_t cost = 0;
        for (;;) {
            uint8_t v = M(hl++);
            n--;
            if (v == a || n == 0) { cost += 16; break; }
            cost += 21;
        }
        uint16_t de = (uint16_t)((M(DCCTSB4_OPERAND) << 8) | c);
        for (int b = 0; b < 8; b++) {
            M(de) = M(hl);
            hl++;
            de = (uint16_t)(de + 0x100);
        }
        ix++;
        T(4 + 19 + 10 + 10 + cost + 4 + 7 + 7 + 8 * 37 - 5 + 10 + 4 + 10);
    }
    T(13 + 7 + 5);
    if (M(CurrentCavernNumber) == 19) {
        /* The Final Barrier: top half comes from the title screen graphic. */
#ifndef MM128
        T(10 + 10 + 10);
        ldir(TitleScreenDataTop, EmptyCavernScreenBuffer, 2048);
#else
        T(10 + 10 + 10 + 7 + 17);
        UnpackData(PackedTitleScreen, EmptyCavernScreenBuffer, 2048, 201);
#endif
    }
    T(10);
}

static void DrawCurrentCavernToScreenBuffer(void)
{
    T(14 + 7 + 13 + 17);
    M(DCCTSB4_OPERAND) = EmptyCavernScreenBuffer >> 8;
    DrawCurrentCavernToScreenBuffer2(EmptyCavernAttributeBuffer);
    T(14 + 7 + 13);
    M(DCCTSB4_OPERAND) = (EmptyCavernScreenBuffer + 0x800) >> 8;
    DrawCurrentCavernToScreenBuffer2(EmptyCavernAttributeBuffer + 0x100);
}

/* ------------------------------------------------------------------------ */
/* AddToTheScore                                                            */
/* ------------------------------------------------------------------------ */
static void AddToTheScore1(uint16_t hl)
{
    for (;;) {
        T(7 + 7 + 12);
        if (M(hl) != 57) { M(hl)++; T(11 + 10); return; }
        /* AddToTheScore: roll the digit over from '9' to '0'. */
        M(hl) = 48;
        hl--;
        T(10 + 6 + 4 + 7 + 12);
        if ((hl & 0xFF) == 42) {
            /* Another 10000 points: extra life and a screen flash. */
            M(ScreenFlashCounter) = 8;
            M(LivesRemaining)++;
            T(7 + 13 + 13 + 4 + 13);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* AnimateCrumblingFloor (HL preserved)                                     */
/* ------------------------------------------------------------------------ */
static void AnimateCrumblingFloor(uint16_t hl)
{
    uint8_t c = (uint8_t)hl;
    uint8_t b = (uint8_t)(((hl >> 8) + 27) | 7);
    T(4 + 4 + 7 + 7 + 4);
    do {
        b--;
        uint8_t a = M((b << 8) | c);
        b++;
        M((b << 8) | c) = a;
        b--;
        T(4 + 7 + 4 + 7 + 4 + 4 + 7 + 12);
    } while (b & 7);
    M((b << 8) | c) = 0;
    b = (uint8_t)(b + 7);
    T(4 + 7 + 4 + 7 + 4 + 7 + 4 + 5);
    if (M((b << 8) | c)) { T(6); return; }
    M(hl + 0x200) = M(BackgroundTile);
    T(13 + 4 + 4 + 7 + 4 + 4 + 10);
}

/* ------------------------------------------------------------------------ */
/* MoveWilly1 / MoveWilly2 - return 1 if Willy is killed (KillWilly1).      */
/* ------------------------------------------------------------------------ */
static uint16_t MW1x8(uint8_t a)
{
    uint8_t l = a & 0xF0;
    uint8_t carry = l >> 7;
    l = (uint8_t)(l << 1);
    uint8_t h = (uint8_t)(92 + carry);
    l |= M(WillysLocInAttrBuffer) & 0x1F;
    uint16_t hl = (uint16_t)((h << 8) | l);
    wr16(WillysLocInAttrBuffer, hl);
    T(7 + 4 + 4 + 8 + 7 + 4 + 13 + 7 + 4 + 4 + 16 + 10);
    return hl;
}

static void make_sound_a(uint8_t a, uint8_t d, uint8_t c)
{
    /* OUT (254),A; XOR 24; LD B,D; DJNZ $; DEC C; JR NZ */
    do {
        out254(a);
        a ^= 24;
        T(7 + 4 + djnz_cost(d) + 4);
        c--;
        T(c ? 12 : 7);
    } while (c);
}

static int MoveWilly2(uint16_t hl);

enum { MW_RET = 0, MW_KILL = 1 };

static int MV2x7(void)
{
    T(13 + 7 + 5);
    if (!(M(WillysDirAndMovFlags) & 2)) { T(6); return MW_RET; }
    T(13 + 7 + 10);
    if (M(WillysDirAndMovFlags) & 1) {
        /* Willy is moving left. */
        T(13 + 4 + 7);
        if (M(WillysAnimationFrame)) {
            M(WillysAnimationFrame)--;
            T(5 + 4 + 13 + 10);
            return MW_RET;
        }
        uint16_t hl = (uint16_t)(rd16(WillysLocInAttrBuffer) - 1 + 0x20);
        T(16 + 6 + 10 + 11 + 13 + 7 + 11);
        if (M(WallTile) == M(hl)) return MW_RET;
        T(13 + 7 + 7);
        if (M(WillysPixelYCoord) & 0x0F) {
            hl = (uint16_t)(hl + 0x20);
            T(13 + 11 + 7 + 11 + 4 + 15);
            if (M(WallTile) == M(hl)) return MW_RET;
            hl = (uint16_t)(hl - 0x20);
        }
        hl = (uint16_t)(hl - 0x20);
        T(13 + 4 + 15 + 7 + 11);
        if (M(WallTile) == M(hl)) return MW_RET;
        wr16(WillysLocInAttrBuffer, hl);
        M(WillysAnimationFrame) = 3;
        T(5 + 16 + 7 + 13 + 10);
        return MW_RET;
    }
    /* Willy is moving right. */
    T(13 + 7 + 7);
    if (M(WillysAnimationFrame) != 3) {
        M(WillysAnimationFrame)++;
        T(4 + 13 + 10);
        return MW_RET;
    }
    uint16_t hl = (uint16_t)(rd16(WillysLocInAttrBuffer) + 2 + 0x20);
    T(16 + 6 + 6 + 10 + 13 + 11 + 7 + 11);
    if (M(WallTile) == M(hl)) return MW_RET;
    T(13 + 7 + 7);
    if (M(WillysPixelYCoord) & 0x0F) {
        hl = (uint16_t)(hl + 0x20);
        T(13 + 11 + 7 + 11 + 4 + 15);
        if (M(WallTile) == M(hl)) return MW_RET;
        hl = (uint16_t)(hl - 0x20);
    }
    hl = (uint16_t)(hl - 0x20);
    T(13 + 4 + 15 + 7 + 11);
    if (M(WallTile) == M(hl)) return MW_RET;
    hl--;
    wr16(WillysLocInAttrBuffer, hl);
    M(WillysAnimationFrame) = 0;
    T(5 + 6 + 16 + 4 + 13 + 10);
    return MW_RET;
}

static int MoveWilly1(void)
{
    uint16_t hl;
    T(13 + 7);
    if (M(AirborneStatusIndicator) == 1) {
        /* Willy is currently jumping. */
        T(7 + 13 + 8 + 7 + 10 + 7 + 7);
        uint8_t a = (uint8_t)((M(JumpingAnimationCounter) & 0xFE) - 8);
        M(WillysPixelYCoord) = (uint8_t)(M(WillysPixelYCoord) + a);
        hl = MW1x8(M(WillysPixelYCoord));
        T(17 + 13 + 7 + 10);
        if (M(WallTile) == M(hl) || (T(6 + 7 + 10), M(WallTile) == M((uint16_t)(hl + 1)))) {
            /* MW1x11: hit a wall tile with Willy's head. */
            M(WillysPixelYCoord) = (uint8_t)((M(WillysPixelYCoord) + 16) & 240);
            T(13 + 7 + 7 + 13 + 17);
            MW1x8(M(WillysPixelYCoord));
            M(AirborneStatusIndicator) = 2;
            M(WillysDirAndMovFlags) &= (uint8_t)~2;
            T(7 + 13 + 10 + 15 + 10);
            return MW_RET;
        }
        uint8_t j = (uint8_t)(M(JumpingAnimationCounter) + 1);
        M(JumpingAnimationCounter) = j;
        a = (uint8_t)(j - 8);
        T(13 + 4 + 13 + 7 + 10);
        if (a & 0x80) { a = (uint8_t)-a; T(8); }
        a++;
        uint8_t d = rotl(a, 3);
        T(4 + 4 * 3 + 4 + 7 + 13);
        make_sound_a(M(BorderColor), d, 0x20);
        j = M(JumpingAnimationCounter);
        T(13 + 7 + 10);
        if (j == 18) {
            /* MW1x9: Willy has just finished a jump. */
            M(AirborneStatusIndicator) = 6;
            T(7 + 13 + 10);
            return MW_RET;
        }
        T(7 + 12);
        if (j != 16) {
            T(7 + 10);
            if (j != 13) return MV2x7();
        }
    }

    /* MW1x4 */
    T(13 + 7 + 12);
    if ((M(WillysPixelYCoord) & 15) == 0) {
        hl = (uint16_t)(rd16(WillysLocInAttrBuffer) + 0x40);
        T(16 + 10 + 11 + 13 + 7 + 10);
        if (M(CrumblingFloorTile) == M(hl)) { T(7); AnimateCrumblingFloor(hl); }
        T(13 + 7 + 12);
        if (M(NastyTile1) == M(hl)) goto MW1x5;
        T(13 + 7 + 12);
        if (M(NastyTile2) == M(hl)) goto MW1x5;
        hl++;
        T(6 + 13 + 7 + 10);
        if (M(CrumblingFloorTile) == M(hl)) { T(7); AnimateCrumblingFloor(hl); }
        T(13 + 7 + 12);
        if (M(NastyTile1) == M(hl)) goto MW1x5;
        T(13 + 7 + 12);
        if (M(NastyTile2) == M(hl)) goto MW1x5;
        uint8_t bg = M(BackgroundTile);
        int z = (bg == M(hl));
        hl--;
        T(13 + 7 + 6 + 10);
        if (!z) return MoveWilly2(hl);
        T(7 + 10);
        if (bg != M(hl)) return MoveWilly2(hl);
    }

MW1x5:
    T(13 + 7 + 10);
    {
        uint8_t a = M(AirborneStatusIndicator);
        if (a == 1) return MV2x7();
        /* Willy is either falling or just about to start falling. */
        M(WillysDirAndMovFlags) &= (uint8_t)~2;
        T(10 + 15 + 4 + 10);
        if (a == 0) {
            /* MW1x10: Willy has just started falling. */
            M(AirborneStatusIndicator) = 2;
            T(7 + 13 + 10);
            return MW_RET;
        }
        a++;
        M(AirborneStatusIndicator) = a;
        uint8_t d = rotl(a, 4);
        T(4 + 13 + 16 + 4 + 7 + 13);
        make_sound_a(M(BorderColor), d, 0x20);
        M(WillysPixelYCoord) = (uint8_t)(M(WillysPixelYCoord) + 8);
        T(13 + 7 + 13);
        MW1x8(M(WillysPixelYCoord));
        return MW_RET;
    }
}

static int MoveWilly2(uint16_t hl)
{
    T(13 + 7 + 10);
    if (M(AirborneStatusIndicator) >= 0x0C) return MW_KILL;   /* KillWilly1 */
    uint8_t e = 0xFF;
    M(AirborneStatusIndicator) = 0;
    T(7 + 4 + 13 + 13 + 7 + 12);
    uint8_t conv = M(ConveyorTile);
    if (conv == M(hl) || (T(6 + 7 + 12), conv == M((uint16_t)(hl + 1)))) {
        /* MV2x1 */
        e = (uint8_t)(M(ConveyorDirection) - 3);
        T(13 + 7 + 4);
    }
    /* MV2x2: keyboard and joystick. */
    uint8_t a;
#ifdef MM128
    /* '1' opens Mihai Novitchi's options menu. */
    T(11 + 10);
    a = in_port(0xF7FE);
    T(7 + 10);
    if (!(a & 1)) { T(7); CheatMenu(); }
    T(10);
#endif
    T(10);
    a = in_port(0xDFFE);                             /* P-O-I-U-Y */
    a = (uint8_t)(((a & 0x1F) | 0x20) & e); e = a;
    T(7 + 7 + 4 + 4 + 10);
    a = in_port(0xFBFE);                             /* Q-W-E-R-T */
    a = (uint8_t)(rotl((uint8_t)(a & 0x1F), 1) | 1);
    e &= a;
    T(7 + 8 + 7 + 4 + 4 + 7);
#ifndef MM128
    a = in_port(0xF7FE);                             /* 1-2-3-4-5 ('5' left) */
#else
    a = in_port(0xE7FE);                             /* 1-5 and 6-0 ('5'/'6' left) */
#endif
    a = (uint8_t)(rotr(a, 1) | 0xF7);
    e &= a;
    T(4 + 7 + 4 + 4 + 7);
    a = in_port(0xEFFE);                             /* 0-9-8-7-6 ('8' right) */
    a |= 0xFB;
    e &= a;
#ifdef MM128
    T(4 + 4);
    a = in_port(0xEFFE);                             /* '7' right */
    a = (uint8_t)(rotr(a, 1) | 0xFB);
    e &= a;
    T(4 + 7 + 4 + 4);
#endif
    T(7 + 4 + 4 + 13 + 4 + 12);
    if (M(KempJoystickIndicator)) {
        T(10 - 5);
        a = in_port(0x001F);
        a = (uint8_t)~(a & 3);
        e &= a;
        T(7 + 4 + 4 + 4);
    }

    /* MV2x3 */
    uint8_t c = 0;
    T(7 + 4 + 7 + 7 + 12);
    if ((e & 42) != 42) { c = 4; T(7 - 5); }
    T(4 + 7 + 7 + 12);
    if ((e & 21) != 21) { c |= 8; T(8 - 5); }
    /* MV2x5 */
    a = (uint8_t)(M(WillysDirAndMovFlags) + c);
    M(WillysDirAndMovFlags) = M(WillyNotMoving0 + a);
    T(13 + 4 + 4 + 7 + 10 + 11 + 7 + 13);

    /* Now check the jump keys. */
    T(10);
    a = in_port(0x7EFE);                             /* SHIFT..V and B..SPACE */
    T(7 + 7 + 12);
    if ((a & 0x1F) != 0x1F) goto MV2x6;
    T(7 - 5);
    a = in_port(0xEFFE);                             /* 0-9-8-7-6 */
#ifndef MM128
    T(7 + 7 + 12);
    if ((a & 0x09) != 0x09) goto MV2x6;              /* '0' or '7' */
#else
    T(8 + 12);
    if (!(a & 0x01)) goto MV2x6;                     /* '0' */
#endif
    T(13 + 4 + 12 - 5);
    if (!M(KempJoystickIndicator)) return MV2x7();
    T(10);
    a = in_port(0x001F);
    T(8 + 12 - 5);
    if (!(a & 0x10)) return MV2x7();

MV2x6:
    M(JumpingAnimationCounter) = 0;
    M(AirborneStatusIndicator) = 1;
    T(4 + 13 + 4 + 13);
    return MV2x7();
}

/* ------------------------------------------------------------------------ */
/* MoveHorzGuardians                                                        */
/* ------------------------------------------------------------------------ */
static void MoveHorzGuardians(void)
{
    T(14 + 10);
    for (uint16_t iy = HorizontalGuardian1;; iy = (uint16_t)(iy + 7)) {
        uint8_t a = M(iy);
        T(19 + 7 + 5);
        if (a == 0xFF) { T(6); return; }
        T(4 + 12);
        if (!a) { T(15 + 12); continue; }
        T(13 + 7 + 12 + 19 - 5);
        if ((uint8_t)(rotr((uint8_t)(M(GameClock) & 4), 3) & a)) { T(5 + 15 + 12); continue; }
        uint8_t f = M(iy + 4);
        T(19 + 7);
        if (f == 3) {
            T(12 + 19 + 19 + 12);
            if (M(iy + 1) == M(iy + 6)) M(iy + 4) = 7;
            else { M(iy + 4) = 0; M(iy + 1)++; T(23); }
        } else if (f == 4) {
            T(7 + 7 + 12 + 19 + 19 + 12);
            if (M(iy + 1) == M(iy + 5)) M(iy + 4) = 0;
            else { M(iy + 4) = 7; M(iy + 1)--; T(23); }
        } else if (f > 4) {
            M(iy + 4)--;
            T(7 + 7 + 7 + 12 + 23 + 12);
        } else {
            M(iy + 4)++;
            T(7 + 7 + 7 + 7 + 23 + 12);
        }
        T(15 + 12);
    }
}

/* ------------------------------------------------------------------------ */
/* MoveDrawLightBeam (Solar Power Generator)                                */
/* ------------------------------------------------------------------------ */
static void MoveDrawLightBeam(void)
{
    uint16_t hl = 0x5C17, de = 0x0020;
    T(10 + 10);
    for (;;) {
        T(13 + 7 + 5);
        if (M(FloorTile) == M(hl)) { T(6); return; }
        T(13 + 7 + 5);
        if (M(WallTile) == M(hl)) { T(6); return; }
        T(7 + 7 + 12);
        if (M(hl) == 0x27) {
            /* The light beam is touching Willy. */
            T(4 + 4 + 4 * 17 + 12);
#ifndef MM128
            DecreaseAirRemaining();
            DecreaseAirRemaining();
            DecreaseAirRemaining();
            DecreaseAirRemaining();
#else
            /* The menu's "infinite air" turns these four CALLs into NOPs. */
            for (int i = 0; i < 4; i++) {
                if (M(PatchBeamCalls + 3 * i) == 0xCD) DecreaseAirRemaining();
                else T(-17 + 12);
            }
#endif
        } else {
            T(13 + 7 + 12);
            if (M(BackgroundTile) != M(hl)) {
                /* Hit a guardian: toggle DE between 32 and -1. */
                uint8_t e = (uint8_t)(de ^ (0x20 ^ 0xFF));
                uint8_t d = (uint8_t)~(de >> 8);
                de = (uint16_t)((d << 8) | e);
                T(4 + 7 + 4 + 4 + 4 + 4 - 5);
            }
        }
        M(hl) = 0x77;
        hl = (uint16_t)(hl + de);
        T(10 + 11 + 12);
    }
}

/* ------------------------------------------------------------------------ */
/* DrawHorzontalGuardians - returns 1 on collision with Willy.              */
/* ------------------------------------------------------------------------ */
static int DrawHorzontalGuardians(void)
{
    T(14);
    for (uint16_t iy = HorizontalGuardian1;; iy = (uint16_t)(iy + 7)) {
        uint8_t a = M(iy);
        T(19 + 7 + 5);
        if (a == 0xFF) { T(6); return 0; }
        T(4 + 12);
        if (!a) { T(10 + 15 + 12); continue; }
        uint16_t hl = (uint16_t)(M(iy + 1) | (M(iy + 2) << 8));
        a &= 0x7F;
        M(hl) = a;
        M((uint16_t)(hl + 1)) = a;
        M((uint16_t)(hl + 0x20)) = a;
        M((uint16_t)(hl + 0x21)) = a;
        uint8_t e = rotr(M(iy + 4), 3);
        uint8_t cav = M(CurrentCavernNumber);
        T(10 + 19 + 19 + 7 + 7 + 6 + 7 + 11 + 7 + 6 + 7 + 7 + 19 + 12 + 4 + 13 + 7 + 12);
        if (!(cav < 7 || cav == 9 || cav == 15)) { e |= 0x80; T(7 + 12 + 7 + 12 + 8); }
        hl = (uint16_t)(M(iy + 1) | (M(iy + 3) << 8));
        T(7 + 19 + 19 + 17);
        if (DrawASprite(hl, (uint16_t)(GuardianGraphicData | e), 1)) return 1;
        T(10 + 10 + 15 + 12);
    }
}

/* ------------------------------------------------------------------------ */
/* SetAttributeMulti                                                        */
/* ------------------------------------------------------------------------ */
static void SetAttributeMulti(uint16_t hl, uint8_t a)
{
    M(hl) = a;
    a = (uint8_t)((M(BackgroundTile) & 0xF8) | M(hl));
    M(hl) = a;
    hl++;               M(hl) = a;
    hl += 0x1F;         M(hl) = a;
    hl++;               M(hl) = a;
    hl += 0x1F;         M(hl) = a;
    hl++;               M(hl) = a;
    T(7 + 13 + 7 + 7 + 7 + 10 + 6 + 7 + 11 + 7 + 6 + 7 + 11 + 7 + 6 + 7 + 10);
}

/* ------------------------------------------------------------------------ */
/* MoveDrawEugene - returns 1 on collision with Willy.                      */
/* ------------------------------------------------------------------------ */
static int MoveDrawEugene(void)
{
    uint8_t a;
    T(13 + 4 + 12);
    if (M(AttrLastItemDrawn) && (T(13 + 4 + 12), M(EugDirOrKongBeastStatus))) {
        /* Eugene is moving up. */
        a = (uint8_t)(M(MultiUseCoordinateStore) - 1);
        T(13 + 4 + 12);
        if (a == 0) goto MoveDrawEugene2;
        M(MultiUseCoordinateStore) = a;
        T(13 + 12);
        goto MoveDrawEugene3;
    }
    /* MoveDrawEugene1: moving down. */
    a = (uint8_t)(M(MultiUseCoordinateStore) + 1);
    T(13 + 4 + 7 + 12);
    if (a != 0x58) {
        M(MultiUseCoordinateStore) = a;
        T(13 + 12 - 5);
        goto MoveDrawEugene3;
    }
MoveDrawEugene2:
    M(EugDirOrKongBeastStatus) ^= 1;
    T(13 + 7 + 13);

MoveDrawEugene3:
    {
        uint8_t e = rotl((uint8_t)(M(MultiUseCoordinateStore) & 0x7F), 1);
        uint16_t de = (uint16_t)(YTable | e);
        uint16_t hl = (uint16_t)(((M((uint16_t)(de + 1))) << 8) | (M(de) | 0x0F));
        T(13 + 7 + 4 + 4 + 7 + 7 + 7 + 4 + 6 + 7 + 4 + 10 + 7 + 17);
        if (DrawASprite(hl, EugeneGraphicData, 1)) return 1;
        a = (uint8_t)(rotl((uint8_t)(M(MultiUseCoordinateStore) & 0x78), 1) | 7);
        /* SCF; RL A */
        uint8_t carry = a >> 7;
        a = (uint8_t)((a << 1) | 1);
        hl = (uint16_t)(((0x5C + carry) << 8) | a);
        T(10 + 13 + 7 + 4 + 7 + 4 + 8 + 4 + 7 + 7 + 4);
        T(13 + 4 + 7 + 12);
        if (M(AttrLastItemDrawn)) {
            a = 7;
        } else {
            a = (uint8_t)(rotr(M(GameClock), 2) & 7);
            T(13 + 4 + 4 + 7 - 5);
        }
        SetAttributeMulti(hl, a);
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* MoveDrawSkyLabs - returns 1 if a Skylab falls on Willy (KillWilly2).     */
/* ------------------------------------------------------------------------ */
static int MoveDrawSkyLabs(void)
{
    T(14);
    for (uint16_t iy = VerticalGuardian1;; iy = (uint16_t)(iy + 7)) {
        T(19 + 7 + 10);
        if (M(iy) == 0xFF) return 0;
        uint8_t a = M(iy + 2);
        T(19 + 19 + 12);
        if (a < M(iy + 6)) {
            M(iy + 2) = (uint8_t)(a + M(iy + 4));
            T(19 + 19 + 12 - 5);
        } else {
            /* MoveDrawSkyLabs3: crashed; start or continue disintegration. */
            M(iy + 1)++;
            T(23 + 19 + 7 + 12);
            if (M(iy + 1) == 8) {
                M(iy + 2) = M(iy + 5);
                M(iy + 3) = (uint8_t)((M(iy + 3) + 8) & 0x1F);
                M(iy + 1) = 0;
                T(19 + 19 + 19 + 7 + 7 + 19 + 19 - 5);
            }
        }
        /* MoveDrawSkyLabs4 */
        uint8_t e = rotl(M(iy + 2), 1);
        uint16_t de = (uint16_t)(YTable | e);
        uint8_t l = (uint8_t)(M(de) + M(iy + 3));
        uint8_t h = M((uint16_t)(de + 1));
        uint16_t hl = (uint16_t)((h << 8) | l);
        uint8_t ge = rotr(M(iy + 1), 3);
        T(19 + 8 + 7 + 7 + 19 + 4 + 6 + 7 + 4 + 19 + 12 + 4 + 7 + 7 + 17);
        if (DrawASprite(hl, (uint16_t)(GuardianGraphicData | ge), 1)) return 1;
        a = M(iy + 2);
        h = (uint8_t)(0x5C + rotl((uint8_t)(a & 0x40), 2));
        l = (uint8_t)((rotl(a, 2) & 0xE0) | M(iy + 3));
        T(10 + 19 + 7 + 8 + 7 + 4 + 19 + 8 + 7 + 19 + 4 + 19 + 17);
        SetAttributeMulti((uint16_t)((h << 8) | l), M(iy));
        T(10 + 15 + 12);
    }
}

/* ------------------------------------------------------------------------ */
/* MoveDrawVerticalGuardians - returns 1 on collision with Willy.           */
/* ------------------------------------------------------------------------ */
static int MoveDrawVerticalGuardians(void)
{
    T(14);
    for (uint16_t iy = VerticalGuardian1;; iy = (uint16_t)(iy + 7)) {
        T(19 + 7 + 5);
        if (M(iy) == 0xFF) { T(6); return 0; }
        M(iy + 1) = (uint8_t)((M(iy + 1) + 1) & ~4);
        uint8_t a = (uint8_t)(M(iy + 2) + M(iy + 4));
        T(23 + 23 + 19 + 19 + 19 + 12);
        if (a < M(iy + 5) || (T(19 + 12), a >= M(iy + 6))) {
            M(iy + 4) = (uint8_t)-M(iy + 4);
            T(19 + 8 + 19);
        } else {
            M(iy + 2) = a;
            T(19 + 12);
        }
        /* MoveDrawVerticalGuardians4 */
        uint8_t e = rotl((uint8_t)(M(iy + 2) & 0x7F), 1);
        uint16_t de = (uint16_t)(YTable | e);
        uint16_t hl = (uint16_t)((M((uint16_t)(de + 1)) << 8) | (uint8_t)(M(de) | M(iy + 3)));
        uint8_t ge = rotr(M(iy + 1), 3);
        T(19 + 7 + 4 + 4 + 7 + 7 + 19 + 4 + 6 + 7 + 4 + 19 + 12 + 4 + 7 + 7 + 17);
        if (DrawASprite(hl, (uint16_t)(GuardianGraphicData | ge), 1)) return 1;
        a = M(iy + 2);
        uint8_t h = (uint8_t)(0x5C + rotl((uint8_t)(a & 0x40), 2));
        uint8_t l = (uint8_t)((rotl(a, 2) & 0xE0) | M(iy + 3));
        T(10 + 19 + 7 + 8 + 7 + 4 + 19 + 8 + 7 + 19 + 4 + 19 + 17);
        SetAttributeMulti((uint16_t)((h << 8) | l), M(iy));
        T(10 + 15 + 12);
    }
}

/* ------------------------------------------------------------------------ */
/* DrawCollectItemsWillyTouching                                            */
/* ------------------------------------------------------------------------ */
static void DrawCollectItemsWillyTouching(void)
{
    M(AttrLastItemDrawn) = 0;
    T(4 + 13 + 14);
    for (uint16_t iy = ItemDef1;; iy = (uint16_t)(iy + 5)) {
        uint8_t a = M(iy);
        T(19 + 7 + 7);
        if (a == 0xFF) { T(5); break; }
        T(4 + 12);
        if (!a) { T(10 * 5 + 12); continue; }
        uint16_t de = (uint16_t)(M(iy + 1) | (M(iy + 2) << 8));
        T(19 + 19 + 7 + 7 + 7 + 12);
        if ((M(de) & 7) == 7) {
            /* Willy is touching this item: add it to his collection. */
            T(10 + 17);
            AddToTheScore1(Score2 + 6);
            M(iy) = 0;
            T(19 + 12 + 10 * 5 + 12);
            continue;
        }
        /* DrawCollectItemsWillyTouching2 */
        uint8_t b = (uint8_t)((a & 0xF8) | 3);
        a = (uint8_t)((a & 3) + b);
        M(iy) = a;
        M(de) = a;
        M(AttrLastItemDrawn) = a;
        de = (uint16_t)((M(iy + 3) << 8) | (de & 0xFF));
        T(19 + 7 + 7 + 4 + 19 + 7 + 4 + 19 + 7 + 13 + 19 + 10 + 7 + 17);
        DrawItem(ItemGraphic, de, 8);
        T(10 * 5 + 12);
    }
    /* DrawCollectItemsWillyTouching4 */
    T(13 + 4 + 5);
    if (M(AttrLastItemDrawn)) { T(6); return; }
    M(PortalDefAttributeByte) |= 0x80;
    T(10 + 15 + 10);
}

/* ------------------------------------------------------------------------ */
/* DrawThePortal - returns 1 if Willy has entered the (flashing) portal.    */
/* ------------------------------------------------------------------------ */
static int DrawThePortal(void)
{
    uint16_t hl = rd16(PortalDefAttributeBuf);
    T(16 + 13 + 4 + 12);
    if (M(WillysLocInAttrBuffer) == (uint8_t)hl
        && (T(13 + 4 + 12), M(WillysLocInAttrBuffer + 1) == (uint8_t)(hl >> 8))
        && (T(13 + 8 + 12), M(PortalDefAttributeByte) & 0x80)) {
        T(10 + 10);
        return 1;   /* POP HL; JP MoveToTheNextCavern */
    }
    uint8_t a = M(PortalDefAttributeByte);
    M(hl) = a;
    M((uint16_t)(hl + 1)) = a;
    M((uint16_t)(hl + 0x20)) = a;
    M((uint16_t)(hl + 0x21)) = a;
    T(13 + 7 + 6 + 7 + 10 + 11 + 7 + 6 + 7 + 10 + 16 + 7);
    DrawASprite(rd16(PortalDefScreenBuf), PortalDefGraphicData, 0);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* MoveConveyorInTheCurrentCavern                                           */
/* ------------------------------------------------------------------------ */
static void MoveConveyorInTheCurrentCavern(void)
{
    uint16_t hl = rd16(ConveyorAddress);
    uint16_t de = hl;
    uint8_t b = M(ConveyorLength);
    uint8_t a, c;
    T(16 + 4 + 4 + 13 + 4 + 13 + 4 + 12);
    if (M(ConveyorDirection) == 0) {
        a = rotl(M(hl), 2);
        hl = (uint16_t)(hl + 0x200);
        c = rotr(M(hl), 2);
        T(7 + 8 + 8 + 4 + 4 + 7 + 8 + 8 - 5);
    } else {
        a = rotr(M(hl), 2);
        hl = (uint16_t)(hl + 0x200);
        c = rotl(M(hl), 2);
        T(7 + 8 + 8 + 4 + 4 + 7 + 8 + 8 + 12);
    }
    do {
        M(de) = a;
        M(hl) = c;
        hl = (uint16_t)((hl & 0xFF00) | (uint8_t)(hl + 1));
        de = (uint16_t)((de & 0xFF00) | (uint8_t)(de + 1));
        T(7 + 7 + 4 + 4 + 13);
    } while (--b);
    T(10 - 5);
}

/* ------------------------------------------------------------------------ */
/* FlipSwitchInKongBeatsCavern - returns 1 (zero flag set) if flipped.      */
/* ------------------------------------------------------------------------ */
static int FlipSwitchInKongBeatsCavern(uint16_t hl)
{
    uint8_t a = (uint8_t)((M(WillysLocInAttrBuffer) + 1) & 0xFE);
    T(13 + 4 + 7 + 4 + 5);
    if (a != (uint8_t)hl) { T(6); return 0; }
    T(13 + 4 + 5);
    if (M(WillysLocInAttrBuffer + 1) != (uint8_t)(hl >> 8)) { T(6); return 0; }
    a = M(ExtraTile + 6);
    hl = (uint16_t)(0x7500 | (hl & 0xFF));
    T(13 + 7 + 7 + 5);
    if (a != M(hl)) { T(6); return 0; }
    M(hl) = 0x08;
    M((uint16_t)(hl + 0x100)) = 0x06;
    M((uint16_t)(hl + 0x200)) = 0x06;
    T(10 + 4 + 10 + 4 + 10 + 4 + 4 + 10);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* MoveDrawKongBeast - returns 1 on collision with Willy.                   */
/* ------------------------------------------------------------------------ */
static int MoveDrawKongBeast(void)
{
    T(10 + 17);
    FlipSwitchInKongBeatsCavern(23558);
    T(13 + 7 + 5);
    if (M(EugDirOrKongBeastStatus) == 2) { T(6); return 0; }
    T(13 + 7 + 10);
    if (M(29958) == 16) goto MoveDrawKongBeast10;

    /* The left-hand switch has been flipped: open up the wall. */
    T(13 + 4 + 12);
    if (M(24433)) {
        uint16_t hl = 32625;
        T(10 - 5);
        for (;;) {
            T(7 + 4 + 7);
            if (M(hl)) {
                /* MoveDrawKongBeast4 */
                M(hl) = 0;
                uint8_t h = (uint8_t)((hl >> 8) ^ 7);
                hl = (uint16_t)((h << 8) | 145);
                M(hl) = 0;
                T(5 + 10 + 7 + 4 + 7 + 4 + 10);
                break;
            }
            hl = (uint16_t)(hl - 0x100);
            T(-7 + 12 + 4 + 4 + 7);
            if ((hl >> 8) == 119) {
                uint8_t bg = M(BackgroundTile);
                M(24433) = bg;
                M(24465) = bg;
                M(HorizontalGuardian2 + 6) = 0x72;
                T(7 + 13 + 13 + 13 + 7 + 13 + 12);
                break;
            }
            T(12 - 7);
        }
    }

    /* MoveDrawKongBeast5: now check the right-hand switch. */
    T(10 + 17 + 7);
    if (FlipSwitchInKongBeatsCavern(23570)) {
        uint8_t bg;
        M(MultiUseCoordinateStore) = 0;
        M(EugDirOrKongBeastStatus) = 1;
        bg = M(BackgroundTile);
        M(24143) = bg;
        M(24144) = bg;
        T(4 + 13 + 4 + 13 + 13 + 13 + 13 + 10 + 7 - 5);
        uint16_t hl = 28751;
        for (int b = 0; b < 8; b++) {
            M(hl) = 0;
            M((uint16_t)(hl + 1)) = 0;
            hl = (uint16_t)(hl + 0x100);
            T(10 + 4 + 10 + 4 + 4 + 13);
        }
    }

    /* MoveDrawKongBeast7 */
    T(13 + 4 + 12);
    if (!M(EugDirOrKongBeastStatus)) goto MoveDrawKongBeast10;
    {
        /* The Kong Beast is falling. */
        uint8_t a = M(MultiUseCoordinateStore);
        T(13 + 7 + 12 - 5);
        if (a == 100) {
            /* MoveDrawKongBeast9: he has fallen into the portal. */
            M(EugDirOrKongBeastStatus) = 2;
            T(7 + 13 + 10);
            return 0;
        }
        a = (uint8_t)(a + 4);
        M(MultiUseCoordinateStore) = a;
        uint8_t c = a;
        T(7 + 13 + 4 + 7 + 13);
        /* Falling sound effect: OUT; XOR 24; LD B,C; DJNZ $; DEC D; JR NZ */
        {
            uint8_t out = M(BorderColor);
            for (uint8_t d = 16; d; ) {
                out254(out);
                out ^= 24;
                T(7 + 4 + djnz_cost(c) + 4);
                d--;
                T(d ? 12 : 7);
            }
        }
        uint8_t e = rotl(c, 1);
        uint16_t de = (uint16_t)(YTable | e);
        uint16_t hl = (uint16_t)((M((uint16_t)(de + 1)) << 8) | (uint8_t)(M(de) | 15));
        uint8_t ge = (uint8_t)((M(GameClock) & 32) | 0x40);
        T(4 + 4 + 4 + 7 + 7 + 7 + 4 + 6 + 7 + 4 + 7 + 13 + 7 + 7 + 4 + 7 + 17);
        DrawASprite(hl, (uint16_t)(GuardianGraphicData | ge), 0);
        T(10 + 17);
        AddToTheScore1(Score2 + 6);
        a = (uint8_t)(M(MultiUseCoordinateStore) & 120);
        hl = (uint16_t)(((23 << 8) | a) << 2);
        hl = (uint16_t)((hl & 0xFF00) | (uint8_t)((hl & 0xFF) | 15));
        T(13 + 7 + 4 + 7 + 11 + 11 + 4 + 7 + 4 + 7 + 10);
        SetAttributeMulti(hl, 6);
        return 0;
    }

MoveDrawKongBeast10:
    {
        /* The Kong Beast is still on the ledge. */
        uint8_t e = (uint8_t)(M(GameClock) & 32);
        T(13 + 7 + 4 + 7 + 10 + 7 + 17);
        if (DrawASprite(24591, (uint16_t)(GuardianGraphicData | e), 1)) return 1;
        M(23599) = 0x44;
        M(23600) = 0x44;
        M(23567) = 0x44;
        M(23568) = 0x44;
        T(10 + 7 + 13 * 4 + 10);
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* CheckSetAttributeForWSIB / CheckSetAttributeForCOBWS /                   */
/* DrawWillyToScreenBuffer - returns 1 if Willy hits a nasty (KillWilly).   */
/* ------------------------------------------------------------------------ */
static int CheckSetAttributeForCOBWS(uint16_t hl, uint8_t c)
{
    T(17 + 13 + 7 + 12);
    if (M(BackgroundTile) == M(hl)) {
        T(4 + 7 + 12 - 5);
        if (c & 0x0F) {
            M(hl) = (uint8_t)(M(BackgroundTile) | 7);
            T(13 + 7 + 7 - 5);
        }
    }
    T(13 + 7 + 10);
    if (M(NastyTile1) == M(hl)) return 1;
    T(13 + 7 + 10);
    if (M(NastyTile2) == M(hl)) return 1;
    T(10);
    return 0;
}

static void DrawWillyToScreenBuffer(void)
{
    uint16_t ix = (uint16_t)(YTable | M(WillysPixelYCoord));
    uint8_t e = rotr((uint8_t)(M(WillysDirAndMovFlags) & 1), 1);
    e |= rotr((uint8_t)(M(WillysAnimationFrame) & 3), 3);
    uint16_t de = (uint16_t)(WillySpriteData | e);
    uint8_t c = M(WillysLocInAttrBuffer) & 0x1F;
    T(13 + 11 + 8 + 13 + 7 + 4 + 4 + 13 + 7 + 4 * 3 + 4 + 4 + 7 + 7 + 13 + 7 + 4);
    for (int b = 0; b < 16; b++) {
        uint8_t l = (uint8_t)(M(ix) | c);
        uint8_t h = M((uint16_t)(ix + 1));
        uint16_t hl = (uint16_t)((h << 8) | l);
        M(hl) |= M(de);
        hl++;
        de++;
        M(hl) |= M(de);
        ix = (uint16_t)(ix + 2);
        de++;
        T(19 + 19 + 4 + 4 + 7 + 7 + 7 + 6 + 6 + 7 + 7 + 7 + 10 + 10 + 6 + 13);
    }
    T(10 - 5);
}

static int CheckSetAttributeForWSIB(void)
{
    uint16_t hl = rd16(WillysLocInAttrBuffer);
    T(16 + 10 + 7);
    if (CheckSetAttributeForCOBWS(hl, 0x0F)) return 1;
    hl++;               T(6);
    if (CheckSetAttributeForCOBWS(hl, 0x0F)) return 1;
    hl += 0x1F;         T(11);
    if (CheckSetAttributeForCOBWS(hl, 0x0F)) return 1;
    hl++;               T(6);
    if (CheckSetAttributeForCOBWS(hl, 0x0F)) return 1;
    uint8_t c = M(WillysPixelYCoord);
    hl += 0x1F;         T(13 + 4 + 11);
    if (CheckSetAttributeForCOBWS(hl, c)) return 1;
    hl++;               T(6);
    if (CheckSetAttributeForCOBWS(hl, c)) return 1;
    T(12);
    DrawWillyToScreenBuffer();
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Theme tune                                                               */
/* ------------------------------------------------------------------------ */
static uint16_t CalcAFAForPianoKey(uint8_t a)
{
    a = (uint8_t)(a - 8);
    a = rotr(a, 3);
    a = (uint8_t)~a;
    a |= 0xE0;
    T(7 + 4 * 3 + 4 + 7 + 4 + 7 + 10);
    return (uint16_t)(0x5900 | a);
}

static int IsEnterOrFireButtonPressed(void)
{
    T(17 + 13 + 4 + 12);
    if (M(KempJoystickIndicator)) {
        uint8_t a = in_port((uint16_t)((M(KempJoystickIndicator) << 8) | 0x1F));
        T(-12 + 11 + 8 + 5 - 7);
        if (a & 0x10) { T(6); return 1; }
    }
    T(10);
    uint8_t a = in_port(ENTER_PORT);
    T(7 + 7 + 10);
    return (a & 1) != 1;
}

static int PlayTheThemeTune(uint16_t iy)
{
    for (;;) {
        uint8_t a = M(iy);
        T(19 + 7 + 5);
        if (a == 0xFF) { T(6); return 0; }
        uint8_t c = a;
        uint8_t b = 0;
        uint8_t d = M(iy + 1);
        T(4 + 7 + 4 + 19 + 4 + 17);
        M(CalcAFAForPianoKey(d)) = 80;
        uint8_t e = M(iy + 2);
        T(10 + 19 + 4 + 17);
        uint16_t hl = CalcAFAForPianoKey(e);
        M(hl) = 40;
        a = (uint8_t)hl;            /* A still holds the LSB from CalcAFAForPianoKey */
        T(10);
        for (;;) {
            out254(a);
            d--;
            T(4);
            if (d == 0) { d = M(iy + 1); a ^= 24; T(7 + 19 + 7); } else T(12);
            e--;
            T(4);
            if (e == 0) { e = M(iy + 2); a ^= 24; T(7 + 19 + 7); } else T(12);
            b--;
            if (b) { T(13); continue; }
            T(8);
            c--;
            T(4);
            if (c) { T(12); continue; }
            T(7);
            break;
        }
        T(17);
        if (IsEnterOrFireButtonPressed()) { T(11); return 1; }
        T(5 + 19 + 17);
        M(CalcAFAForPianoKey(M(iy + 1))) = 56;
        T(10 + 19 + 17);
        M(CalcAFAForPianoKey(M(iy + 2))) = 56;
        iy = (uint16_t)(iy + 3);
        T(10 + 10 * 3 + 12);
    }
}

#ifdef MM128
/* ------------------------------------------------------------------------ */
/* Unpack (37835) - Manic Miner 128's LZ unpacker.                          */
/* HL = packed data, DE = destination, BC = bytes to produce, A = 0 to      */
/* enable the hooks that make a 1024-byte cavern stream continue from      */
/* 0x6000 at 0x8000 (attribute buffer 0x5E00, then cavern definition), or   */
/* 201 (RET) to disable them.                                               */
/* ------------------------------------------------------------------------ */
static inline void unpack_hook_de(uint16_t *de)
{
    T(17);                                    /* CALL 37962 */
    if (M(UnpackHookDE) == 0xC9) { T(10); return; }
    T(4 + 11 + 4 + 7 + 7);
    if ((*de >> 8) == 96) { *de = 32768; T(10 - 5); }
    T(10 + 10);
}

static inline void unpack_hook_hl(uint16_t *hl)
{
    T(17);                                    /* CALL 37974 */
    if (M(UnpackHookHL) == 0xC9) { T(10); return; }
    T(4 + 11 + 4 + 7 + 7);
    if ((*hl >> 8) == 96) { *hl = 32768; T(10 - 5); }
    T(10 + 10);
}

static void UnpackData(uint16_t hl, uint16_t de, uint16_t bc, uint8_t a)
{
    M(UnpackHookDE) = a;
    M(UnpackHookHL) = a;
    M(UnpackMarker) = M(hl);
    hl++;
    T(13 + 13 + 7 + 6 + 13);
    for (;;) {
        /* 37846 */
        uint8_t v = M(hl);
        T(7 + 17 + 7 + 10 + 7);
        if (v == M(UnpackMarker)) {
            /* 37862: escape */
            hl++;
            v = M(hl);
            T(6 + 7 + 4 + 12 - 5);
            if (v == 0) {
                T(13 + 12);
                /* 37852: LDI copies the byte at HL (the 0) */
            } else {
                /* 37872: back-reference */
                uint16_t ix = bc;
                bc = v & 63;
                uint16_t iy = hl;
                hl = (uint16_t)(hl + 2);
                uint16_t next = hl;
                uint16_t offset = (uint16_t)((M(iy) | (M((uint16_t)(iy + 1)) << 8)) >> 6);
                uint16_t src = (uint16_t)(de - offset);
                T(15 + 14 + 7 + 7 + 4 + 15 + 14 + 6 + 6 + 11 + 11 + 11 + 10 + 19 + 19 + 11 + 7
                  + 6 * (8 + 8 + 13) - 5 + 10 + 4 + 4 + 15 + 13 + 4 + 7);
                if (M(UnpackHookDE) == 0) {
                    uint8_t h = (uint8_t)(src >> 8);
                    T(4 + 7 + 17);
                    if (h == 127) { h = 95; T(7 + 4 + 10); }
                    T(7 + 10);
                    if (h == 126) { h = 94; T(7 + 4 + 10 + 7); }
                    src = (uint16_t)((h << 8) | (src & 0xFF));
                }
                T(10);
                for (;;) {
                    /* 37927: LDI */
                    M(de) = M(src);
                    de++; src++; bc--;
                    T(16);
                    unpack_hook_de(&de);
                    unpack_hook_hl(&src);
                    ix--;
                    T(10 + 8 + 8 + 4 + 12);
                    if (ix == 0) { T(-5 + 10 + 10); return; }
                    T(4 + 4 + 12);
                    if (bc == 0) break;
                }
                hl = next;
                bc = ix;
                T(10 + 15 + 10 + 12);
                /* 37857 */
                T(4 + 4 + 12);
                if (bc == 0) { T(10); return; }
                continue;
            }
        }
        /* 37852: LDI + hook */
        M(de) = M(hl);
        de++; hl++; bc--;
        T(16);
        unpack_hook_de(&de);
        /* 37857 */
        T(4 + 4 + 12);
        if (bc == 0) { T(-5 + 10); return; }
    }
}

/* ------------------------------------------------------------------------ */
/* Mihai Novitchi's in-game menu (38051), opened with '1'.                  */
/* The options work by patching the game's own code, as in the original:    */
/*   lives:  DEC (HL) at PatchLivesDec <-> NOP                              */
/*   air:    CALL DecreaseAirRemaining at PatchAirCall <-> LD A,1 / OR A,   */
/*           and the light beam's four CALLs at PatchBeamCalls <-> NOPs     */
/*   music:  MusicOn flag, EI, or DI after resetting the player             */
/* ------------------------------------------------------------------------ */
static uint16_t MenuScreenAddr(uint8_t a)          /* 38383 */
{
    uint8_t h = (uint8_t)((a & 24) | 64);
    uint8_t l = rotr((uint8_t)(a & 7), 3);
    T(11 + 7 + 7 + 4 + 10 + 7 + 4 * 3 + 4 + 10);
    return (uint16_t)((h << 8) | l);
}

static void MenuPrint(uint8_t row, uint8_t len, uint16_t ix)    /* 38371 */
{
    T(17);
    uint16_t hl = MenuScreenAddr(row);
    T(10 + 11 + 4 + 17);
    PrintAMessage(ix, (uint16_t)(hl + 5), len);
    T(10);
}

static int MenuToggle(uint16_t hl)                  /* 38345: 1 = now YES */
{
    T(7 + 7 + 12);
    if (M(hl) == 78) {
        M(hl) = 89; M(hl + 1) = 69; M(hl + 2) = 83;    /* "YES" */
        T(-5 + 10 + 6 + 10 + 6 + 10 + 4 + 10);
        return 1;
    }
    M(hl) = 78; M(hl + 1) = 79; M(hl + 2) = 32;        /* "NO " */
    T(10 + 6 + 10 + 6 + 10 + 4 + 4 + 10);
    return 0;
}

static void CheatMenu(void)
{
    /* Paint the menu box attributes: 7 rows x 24 cells of 71 from (2,4). */
    uint16_t hl = 22596;
    T(10 + 7);
    for (int b = 0; b < 7; b++) {
        T(11 + 11 + 10 + 6 + 10 + 10);
        fill(hl, 71, 23);
        hl = (uint16_t)(hl + 32);
        T(10 + 11 + 10 + 13);
    }
    /* Draw the box outline. */
    T(7 + 7);
    for (uint8_t c = 0; c < 7; c++) {
        T(11 + 4 + 7 + 17);
        uint16_t row = MenuScreenAddr((uint8_t)(c + 2));
        T(7);
        for (uint8_t b = 8; b; b--) {
            uint16_t p = (uint16_t)(row + 4);
            uint8_t d = 0;
            T(11 + 11 + 10 + 11 + 7 + 4 + 7 + 12);
            if (c == 0 && b == 8) d = 255;
            else if (c == 6 && b == 1) d = 255;
            T(4 + 7 + 7 + 7 + 7 + 7);
            for (uint8_t k = 24; k; k--) {
                uint8_t e = d;
                if (k == 24) e |= 128;
                if (k == 1) e |= 1;
                M(p) = e;
                p++;
                T(4 + 4 + 7 + 12 + 4 + 7 + 12 + 7 + 6 + 13);
            }
            row = (uint16_t)(row + 0x100);
            T(10 + 4 + 10 + 13);
        }
        T(10 + 4 + 13);
    }

    for (;;) {
        /* 38155: print the options */
        T(7 + 7 + 14);
        MenuPrint(3, 21, MenuText);
        T(7 + 7 + 14);
        MenuPrint(4, 19, MenuText + 21);
        T(7 + 7 + 14);
        MenuPrint(5, 12, MenuText + 40);
        T(7 + 7 + 14);
        MenuPrint(7, 13, MenuText + 52);
        /* Pause: LD BC,28672 / DEC BC / LD A,B / OR C / JR NZ */
        T(10 + 28672u * 26 - 5);

        for (;;) {
            /* 38207: read keys 1-2-3-4-5 */
            T(10);
            uint8_t keys = in_port(0xF7FE);
            T(4 + 7 + 4 + 12);
            if (!(keys & 1)) {
                T(10 + 17);
                if (MenuToggle(MenuLivesYesNo)) M(PatchLivesDec) = 0;
                else M(PatchLivesDec) = 53;
                T(12 + 10 + 10 + 12);
                break;
            }
            T(7 + 4 + 12);
            if (!(keys & 2)) {
                T(10 + 17);
                if (MenuToggle(MenuAirYesNo)) {
                    /* 38281: infinite air */
                    M(PatchAirCall) = 62; M(PatchAirCall + 1) = 1; M(PatchAirCall + 2) = 183;
                    T(12 + 10 + 10 + 6 + 10 + 6 + 10 + 10 + 10 + 11 + 10 + 6 + 10);
                    fill(PatchBeamCalls, 0, 11);
                    T(10);
                } else {
                    uint16_t a = PatchAirCall;
                    M(a) = 205; M(a + 1) = DecreaseAirRemaining_CODE & 0xFF; M(a + 2) = DecreaseAirRemaining_CODE >> 8;
                    for (int i = 0; i < 4; i++) {
                        uint16_t q = (uint16_t)(PatchBeamCalls + 3 * i);
                        M(q) = 205; M(q + 1) = DecreaseAirRemaining_CODE & 0xFF; M(q + 2) = DecreaseAirRemaining_CODE >> 8;
                    }
                    T(7 + 10 + 10 + 10 + 6 + 7 + 6 + 7 + 10 + 7 + 4 * (10 + 6 + 7 + 6 + 7 + 6 + 13) + 12);
                }
                break;
            }
            T(7 + 4 + 12);
            if (!(keys & 4)) {
                T(10 + 17);
                if (MenuToggle(MenuMusicYesNo)) {
                    M(MusicOn) = 1;
                    zx_iff = 1;
                    T(12 + 7 + 13 + 4 + 10);
                } else {
                    M(MusicOn) = 0;
                    zx_iff = 0;
                    T(7 + 13 + 4 + 10);
                    /* JP (MenuMusicOffJump) -> 40016: CALL 49152; DI; JP 38155 */
                    T(17);
                    st_init();
                    zx_iff = 0;
                    T(4 + 10);
                }
                break;
            }
            T(7 + 4 + 10);
            if (!(keys & 8)) { T(10); return; }       /* '4': resume */
        }
    }
}

/* ------------------------------------------------------------------------ */
/* IM2 interrupt: vector table at 39680 -> 40092: push everything,          */
/* CALL 49158 (Sound Tracker play), pop everything, EI, RET.                */
/* ------------------------------------------------------------------------ */
static void mm128_interrupt(void)
{
    if (zx_im != 2) {
        /* IM 1 would run the ROM's handler; only possible in the few
         * T-states between the player's EI and the game's DI on the very
         * first title screen. Nothing of the game's state is involved. */
        return;
    }
    zx_iff = 0;
    T(19 + 11 * 4 + 15 * 2 + 17 + 10);
    st_play();
    if (mm_music_tick_hook) mm_music_tick_hook();
    T(14 * 2 + 10 * 4 + 4 + 10);
    zx_iff = 1;
}
#endif

/* ------------------------------------------------------------------------ */
/* The game: Start, main loop, losing a life, game over, next cavern.       */
/* ------------------------------------------------------------------------ */
void mm_run(void)
{
    uint8_t a;
    uint16_t hl;

    /* AppEntry: DI; LD SP,MemTop; JP Start */
    zx_iff = 0;
#ifdef MM128
    zx_int_hook = mm128_interrupt;
#endif
    T(4 + 10 + 10);

Start:
    mm_in_title = 1;
    M(CurrentCavernNumber) = 0;
    M(KempJoystickIndicator) = 0;
    M(GameModeIndicator) = 0;
    M(MusicNoteIndex) = 0;
    M(ScreenFlashCounter) = 0;
    M(LivesRemaining) = 2;
    M(MusicFlags) |= 1;
    T(4 + 13 * 5 + 7 + 13 + 10 + 15);

    /* Prepare the screen. */
    T(10 + 10 + 10);
    fill(0x4000, 0x00, 0x17FF);
#ifndef MM128
    T(10 + 10 + 10);
    ldir(TitleScreenDataTop, 0x4000, 0x1000);
    T(10 + 10 + 7 + 17);
    DrawASprite(0x483D, WillySpriteData1, 0);
    T(10 + 10 + 10);
    ldir(TheFinalBarrierData, 0x5800, 0x100);
    T(10 + 10);
    ldir(BottomAttributes, 0x5900, 0x200);
#else
    T(10 + 10 + 10 + 7 + 17);
    UnpackData(PackedTitleScreen, 0x4000, 0x1000, 201);
    T(10 + 10 + 7 + 17);
    DrawASprite(0x483D, WillySpriteData1, 0);
    T(10 + 10 + 10 + 7 + 17);
    UnpackData(PackedTitleAttributes, 0x5800, 768, 201);
#endif

    /* Check whether there is a joystick connected. */
    T(10 + 4 + 4);
    zx_iff = 0;                                      /* DI */
    a = 0;
    for (int i = 0; i < 256; i++) {
        a |= in_port((uint16_t)(((uint8_t)(0 - i)) << 8 | 0x1F));
        T(4 + 13);
    }
    T(-5 + 7 + 12);
    if (!(a & 0x20)) { M(KempJoystickIndicator) = 1; T(7 + 13 - 5); }

    /* Play the theme tune and check for keypresses. */
    T(14 + 17);
#ifdef MM128
    /* CALL (TitleTuneCall) -> 40023: CALL 49152 (reset the AY player);
     * DI; CALL PlayTheThemeTune; RET */
    T(17);
    st_init();
    zx_iff = 0;
    T(4 + 17);
    a = (uint8_t)PlayTheThemeTune(TitleScreenTuneData);
    T(10);
    if (a) { T(10); goto Start6; }
#else
    if (PlayTheThemeTune(TitleScreenTuneData)) { T(10); goto Start6; }
#endif
    T(10 + 4 + 13);
    M(MultiUseCoordinateStore) = 0;

    for (;;) {
        /* Start4: scroll the banner and animate Willy. */
        a = M(MultiUseCoordinateStore);
#ifndef MM128
        T(13 + 14 + 8 + 10 + 7 + 17);
        PrintAMessage((uint16_t)(TitleScreenBanner | a), 0x5060, 0x20);
#else
        /* The banner is no longer page-aligned: IX = banner + index. */
        T(13 + 14 + 8 + 8 + 7 + 8 + 8 + 10 + 7 + 17);
        PrintAMessage((uint16_t)(TitleScreenBanner + a), 0x5060, 0x20);
#endif
        a = (uint8_t)(M(MultiUseCoordinateStore) & 0x06);
        a = rotr(a, 3);
        T(13 + 7 + 4 * 3 + 4 + 7 + 10 + 7 + 17);
        DrawASprite(0x483D, (uint16_t)(WillySpriteData | a), 0);
        /* Pause for about 0.1s: LD BC,100 / DJNZ / DEC C / JR NZ. */
        T(10);
        for (uint8_t c = 100; c; c--)
            T(djnz_cost(0) + 4 + 12);
        T(-5 + 10);
        a = in_port(ENTER_PORT);
        T(7 + 7 + 12);
        if ((a & 1) != 1) goto Start6;
        a = (uint8_t)(M(MultiUseCoordinateStore) + 1);
        M(MultiUseCoordinateStore) = a;
        T(13 + 4 + 7 + 13 + 12);
        if (a == 224) break;
    }
    M(GameModeIndicator) = 0x40;      /* demo mode */
    T(7 + 13);

Start6:
    mm_in_title = 0;
    /* Initialise the score. */
    T(10 + 10 + 10);
    fill(Score1, 0x30, 9);

Start7:
    /* Entry point used when teleporting into a cavern or reinitialising the
     * current cavern after Willy has lost a life. */
#ifndef MM128
    a = (uint8_t)((M(CurrentCavernNumber) << 2) + (CentralCavernData >> 8));
    hl = (uint16_t)(a << 8);
    T(13 + 8 + 8 + 7 + 4 + 7 + 10 + 10);
    ldir(hl, EmptyCavernAttributeBuffer, 0x200);
    T(10 + 10);
    ldir((uint16_t)(hl + 0x200), CavernName, 0x200);
#else
    /* Restart the AY tune, point the IM2 vector table at the player's
     * interrupt handler and enable interrupts if the music is on. */
    T(17);
    st_init();
    T(10 + 10 + 10 + 10);
    fill(IM2Table, 156, 256);
    zx_iff = 0;                                      /* DI */
    zx_im = 2;                                       /* LD A,155; LD I,A; IM 2 */
    T(4 + 7 + 9 + 8 + 13 + 4 + 12);
    if (M(MusicOn)) { zx_iff = 1; T(4 - 5); }        /* EI */
    /* Unpack the cavern: 512 bytes to 0x5E00, then 512 to 0x8000. */
    a = M(CurrentCavernNumber);
    {
        uint16_t entry = (uint16_t)(CavernTable + (uint8_t)(a + a + a));
        hl = (uint16_t)(M(entry) | (M((uint16_t)(entry + 1)) << 8));
    }
    T(13 + 4 + 8 + 4 + 4 + 7 + 10 + 11 + 15 + 14 + 19 + 19 + 10 + 7 + 10 + 17);
    UnpackData(hl, EmptyCavernAttributeBuffer, 1024, 0);
#endif
    T(17);
    DrawCurrentCavernToScreenBuffer();
    T(10 + 10 + 10);
    fill(20480, 0x00, 2047);
    T(14 + 7 + 10 + 17);
    PrintAMessage(CavernName, 20480, 0x20);
    T(14 + 7 + 10 + 17);
    PrintAMessage(AirText, 20512, 3);
    T(7);
    for (a = 82; a != 0x56; a++) {
        /* Start8: draw a row of pixels of the air bar. */
        uint8_t c = (uint8_t)(M(RemainingAirSupply) - 0x24);
        T(4 + 4 + 7 + 7 + 4 + 13 + 7 + 4 + 4 + 7);
        fill((uint16_t)((a << 8) | 0x24), 0xFF, c);
        T(4 + 7 + 12);
    }
    T(14 + 10 + 7 + 17);
    PrintAMessage(HighScoreText, 0x5060, 0x20);
    T(13 + 7);
    zx_out(0x00FE, M(BorderColor));
    T(12 + 13 + 4 + 12);
    if (M(GameModeIndicator)) {
        M(GameModeIndicator) = 0x40;
        T(7 + 13 - 5);
    }

    /* -------------------------------------------------------------------- */
    /* Main loop                                                            */
    /* -------------------------------------------------------------------- */
    for (;;) {
        /* MainLoop (0x870E) */
        if (mm_mainloop_hook) mm_mainloop_hook();
        a = M(LivesRemaining);
        hl = 20640;
        T(13 + 10 + 4 + 12);
        if (a) {
            /* Draw the remaining lives at the bottom of the screen. */
            uint8_t b = a;
            T(4 - 5);
            do {
                uint8_t e = (uint8_t)(rotl(M(MusicNoteIndex), 3) & 0x60);
                T(7 + 11 + 11 + 13 + 4 * 3 + 7 + 4 + 7 + 17);
                DrawASprite(hl, (uint16_t)(WillySpriteData | e), 0);
                hl = (uint16_t)(hl + 2);
                T(10 + 10 + 6 + 6 + 13);
            } while (--b);
            T(-5);
        }
        /* MainLoop3: draw a boot if cheat mode has been activated. */
        T(13 + 7 + 12);
        if (M(KeyCounter) == CHEAT_DONE) {
            T(10 + 7 + 17 - 5);
            DrawASprite(hl, BootGraphicData, 0);
        }

        /* MainLoop4: prepare the screen and attribute buffers. */
        T(10 + 10 + 10);
        ldir(EmptyCavernAttributeBuffer, AttributeBufferCWGI, 0x200);
        T(10 + 10 + 10);
        ldir(EmptyCavernScreenBuffer, ScreenBufferCWGI, 0x1000);
        T(17);
        MoveHorzGuardians();
        T(13 + 4 + 10);
        if (!M(GameModeIndicator)) {
            T(17 - 10);
            if (MoveWilly1()) goto KillWilly;
        }
        T(13 + 4 + 10);
        if (!M(GameModeIndicator)) {
            T(17 - 10);
            if (CheckSetAttributeForWSIB()) goto KillWilly;
        }
        T(17);
        if (DrawHorzontalGuardians()) goto KillWilly;
        T(17);
        MoveConveyorInTheCurrentCavern();
        T(17);
        DrawCollectItemsWillyTouching();
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) == 4) {
            T(17 - 10);
            if (MoveDrawEugene()) goto KillWilly;
        }
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) == 13) {
            /* JP Z,MoveDrawSkyLabs ... JP MainLoop5 */
            if (MoveDrawSkyLabs()) goto KillWilly;
            goto MainLoop5;
        }
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) >= 8) {
            T(17 - 10);
            if (MoveDrawVerticalGuardians()) goto KillWilly;
        }
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) == 7) {
            T(17 - 10);
            if (MoveDrawKongBeast()) goto KillWilly;
        }
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) == 11) {
            T(17 - 10);
            if (MoveDrawKongBeast()) goto KillWilly;
        }
        T(13 + 7 + 10);
        if (M(CurrentCavernNumber) == 18) {
            T(17 - 10);
            MoveDrawLightBeam();
        }
MainLoop5:
        T(17);
        if (DrawThePortal()) goto MoveToTheNextCavern;
        goto MainLoop6;

KillWilly:
        /* KillWilly / KillWilly1 / KillWilly2 */
        M(AirborneStatusIndicator) = 0xFF;
        T(7 + 13 + 10);

MainLoop6:
        /* Copy the screen buffer to the display file. */
        T(10 + 10 + 10);
        ldir(ScreenBufferCWGI, 0x4000, 0x1000);
        a = M(ScreenFlashCounter);
        T(13 + 4 + 12);
        if (a) {
            a--;
            M(ScreenFlashCounter) = a;
            a = (uint8_t)(rotl(a, 3) & 0x38);
            T(4 + 13 + 4 * 3 + 7 + 10 + 10 + 10 - 5);
            fill(AttributeBufferCWGI, a, 511);
        }
        /* MainLoop7 */
        T(10 + 10 + 10);
        ldir(AttributeBufferCWGI, 0x5800, 0x200);
        T(14 + 10 + 7 + 17);
        PrintAMessage(Score2 + 3, 20602, 6);
        T(14 + 10 + 7 + 17);
        PrintAMessage(HighScore, 20587, 6);
#ifndef MM128
        T(17 + 10);
        if (DecreaseAirRemaining()) goto MainLoop19;
#else
        /* The menu's "infinite air" replaces this CALL with LD A,1 / OR A. */
        if (M(PatchAirCall) == 0xCD) {
            T(17 + 10);
            if (DecreaseAirRemaining()) goto MainLoop19;
        } else {
            T(7 + 4 + 10);
        }
#endif

        /* SHIFT and SPACE together quit the game. */
        {
            T(10);
            uint8_t e = in_port(0xFEFE);
            T(4);
            a = in_port(0x7FFE);
            T(4 + 7 + 10);
            if (!((a | e) & 1)) goto Start;
        }

        /* A-S-D-F-G pause the game. */
        a = in_port(0xFDFE);
        T(7 + 7 + 12);
        if ((a & 0x1F) != 0x1F) {
            do {
                T(7);
                a = in_port(0x02FE);
                T(7 + 7 + 12);
            } while ((a & 0x1F) == 0x1F);
        }

        /* MainLoop9: has Willy had a fatal accident? */
        T(13 + 7 + 10);
        if (M(AirborneStatusIndicator) == 0xFF) goto MainLoop19;

        /* H-J-K-L-ENTER toggle the in-game music. */
        T(7 + 10);
        a = in_port(0xBFFE);
        T(7 + 7 + 12);
        if ((a & 0x1F) != 0x1F) {
            T(12);
            if (!(M(MusicFlags) & 1)) {
                M(MusicFlags) ^= 3;
                T(7 + 7 + 7 + 12);
            }
        } else {
            M(MusicFlags) &= (uint8_t)~1;
            T(15);
        }
        /* MainLoop11 */
        T(12 + 12);
#ifdef MM128
        /* The beeper tune is gone (the AY plays from the interrupt); the
         * H-J-K-L-ENTER toggle still flips the flag but nothing reads it:
         * BIT 1,(HL) / JR NZ,next. */
        T(-12 + 7);
        if (0) {
#else
        if (!(M(MusicFlags) & 2)) {
#endif
            /* Play a note of the in-game music. */
            a = (uint8_t)(M(MusicNoteIndex) + 1);
            M(MusicNoteIndex) = a;
            uint16_t note = (uint16_t)(InGameTuneData + ((a & 126) >> 1));
            uint8_t out = M(BorderColor);
            uint8_t e = M(note);
            uint8_t b = 0, c = 3;
            T(13 + 4 + 13 + 7 + 4 + 4 + 7 + 10 + 11 + 13 + 7 + 10);
            for (;;) {
                out254(out);
                e--;
                T(4);
                if (e == 0) { e = M(note); out ^= 24; T(7 + 7 + 7); } else T(12);
                b--;
                if (b) { T(13); continue; }
                T(8 + 4);
                if (--c) { T(12); continue; }
                T(7);
                break;
            }
        }

        /* MainLoop14: demo mode. */
        T(13 + 4 + 12);
        if (M(GameModeIndicator)) {
            a = (uint8_t)(M(GameModeIndicator) - 1);
            T(4 + 10);
            if (a == 0) goto MainLoop19;
            M(GameModeIndicator) = a;
            T(13 + 10);
            a = in_port(0x00FE);
            T(7 + 7 + 10);
            if ((a & 0x1F) != 0x1F) goto Start;
            T(13 + 4 + 12);
            if (M(KempJoystickIndicator)) {
                a = in_port((uint16_t)((M(KempJoystickIndicator) << 8) | 0x1F));
                T(-12 + 11 + 4 + 10 - 5);
                if (a) goto Start;
            }
        }

        /* MainLoop15: the teleport keys. */
        T(10);
        a = in_port(0xEFFE);
        T(8 + 10);
        if (!(a & TELEPORT_BIT)) {
            T(13 + 7 + 10);
            if (M(KeyCounter) == CHEAT_DONE) {
                T(7);
                a = in_port(0xF7FE);
                a = (uint8_t)((~a) & 0x1F);
                T(4 + 7 + 7 + 10);
                if (a < 20) {
                    M(CurrentCavernNumber) = a;
                    T(13 + 10);
                    goto Start7;
                }
            }
        }

        /* MainLoop16: the cheat-code keys (6031769 / TYPEWRITER). */
        T(13 + 7 + 10);
        if (M(KeyCounter) == CHEAT_DONE) continue;
        {
            a = rotl(M(KeyCounter), 1);
            uint16_t ix = (uint16_t)(KP6031769x6 + a);
            T(4 + 4 + 7 + 14 + 15 + 10);
            a = in_port(CHEAT_ROW1) & 0x1F;
            T(7 + 19 + 12);
            if (a == M(ix)) {
                /* MainLoop17 */
                T(7);
                a = in_port(CHEAT_ROW2) & 0x1F;
                T(7 + 19 + 12);
                if (a == M((uint16_t)(ix + 1))) {
                    M(KeyCounter)++;
                    T(13 + 4 + 13 + 10);
                    continue;
                }
                T(7 + 10);
                if (a == 0x1F) continue;
                T(19 + 10);
                if (a == M((uint16_t)(ix - 1))) continue;
                M(KeyCounter) = 0;
                T(4 + 13 + 10);
                continue;
            }
            T(7 + 10);
            if (a == 0x1F) continue;
            T(19 + 10);
            if (a == M((uint16_t)(ix - 2))) continue;
            M(KeyCounter) = 0;
            T(4 + 13 + 10);
            continue;
        }

MainLoop19:
        /* The air has run out, Willy has had a fatal accident, or it's demo
         * mode and time to show the next cavern. */
        T(13 + 4 + 10);
        if (M(GameModeIndicator)) goto MoveToTheNextCavern;
        a = 0x47;
        T(7);
        do {
            /* MainLoop20 */
            T(10 + 10 + 10);
            fill(0x5800, a, 511);
            uint8_t e = a;
            uint8_t d = (uint8_t)(rotl((uint8_t)((~a) & 7), 3) | 7);
            uint8_t c = rotr(e, 3);
            T(4 + 4 + 7 + 4 * 3 + 7 + 4 + 4 + 8 * 3 + 7 + 4);
            make_sound_a(0, d, c);
            a = (uint8_t)(e - 1);
            T(4 + 4 + 7 + 12);
        } while (a != 0x3F);

        /* Finally, check whether any lives remain. */
        T(10 + 7 + 4 + 10);
        if (!M(LivesRemaining)) goto DisplayGameOver;
#ifdef MM128
        /* The menu's "infinite lives" replaces this DEC (HL) with NOP. */
        if (M(PatchLivesDec) == 0x35)
#endif
        M(LivesRemaining)--;
        T(11 + 10);
        goto Start7;
    }

DisplayGameOver:
    if (mm_game_over_hook) mm_game_over_hook();
    {
        /* First check whether we have a new high score. */
        uint16_t h = HighScore, de = Score2 + 3;
        T(10 + 10 + 7);
        for (int b = 6; b > 0; b--) {
            T(7 + 7 + 10 + 10);
            if (M(de) < M(h)) goto DisplayGameOver4;
            if (M(de) != M(h)) break;
            h++; de++;
            T(6 + 6 + 13);
        }
        /* DisplayGameOver3 */
        T(10 + 10 + 10);
        ldir(Score2 + 3, HighScore, 6);
    }
DisplayGameOver4:
    T(10 + 10 + 10);
    fill(0x4000, 0x00, 0x0FFF);
    M(MultiUseCoordinateStore) = 0;
    T(4 + 13 + 10 + 10 + 7 + 17);
    DrawASprite(18575, WillySpriteData1, 0);
    T(10 + 10 + 7 + 17);
    DrawASprite(18639, PlinthGraphicData, 0);

    do {
        /* DisplayGameOver5: the boot descends onto the plinth. */
        uint8_t c = M(MultiUseCoordinateStore);
        uint16_t bc = (uint16_t)(YTable | c);
        uint8_t l = (uint8_t)(M(bc) | 15);
        uint8_t h = (uint8_t)(M((uint16_t)(bc + 1)) - 0x20);
        T(13 + 4 + 7 + 7 + 7 + 4 + 6 + 7 + 7 + 4 + 10 + 7 + 17);
        DrawASprite((uint16_t)((h << 8) | l), BootGraphicData, 0);
        uint8_t e = (uint8_t)~M(MultiUseCoordinateStore);
        T(13 + 4 + 4 + 4 + 10);
        /* Rising sound effect: OUT; XOR 24; LD B,E; DJNZ $; DEC C; JR NZ */
        make_sound_a(0, e, 0x40);
        a = (uint8_t)(rotl((uint8_t)(M(MultiUseCoordinateStore) & 12), 1) | 71);
        T(10 + 10 + 10 + 13 + 7 + 4 + 7 + 7);
        fill(0x5800, a, 511);
        a = (uint8_t)(M(MultiUseCoordinateStore) + 4);
        M(MultiUseCoordinateStore) = a;
        T(13 + 7 + 13 + 7 + 12);
    } while (a != 196);

    /* Print "Game Over". */
    T(14 + 7 + 10 + 17);
    PrintAMessage(GameText, 16586, 4);
    T(14 + 7 + 10 + 17);
    PrintAMessage(OverText, 16594, 4);
    T(10 + 7);
    {
        /* Make "Game Over" glisten for about 1.57s. */
        uint8_t c = 0;
        for (uint8_t d = 6; d; d--) {
            do {
                T(djnz_cost(0));
                a = c;
                static const uint16_t cells[8] = { 22730, 22731, 22732, 22733, 22738, 22739, 22740, 22741 };
                for (int i = 0; i < 8; i++) {
                    if (i) a++;
                    a = (uint8_t)((a & 7) | 0x40);
                    M(cells[i]) = a;
                }
                T(4 + (7 + 7 + 13) * 8 + 4 * 7);
                c--;
                T(4 + 12);
            } while (c);
            T(4 + 12 - 5);
        }
    }
    T(10);
    goto Start;

MoveToTheNextCavern:
    a = (uint8_t)(M(CurrentCavernNumber) + 1);
    T(13 + 4 + 7 + 12);
    if (a == 20) {
        T(13 + 4 + 10);
        if (!M(GameModeIndicator)) {
            T(13 + 7 + 12);
            if (M(KeyCounter) != CHEAT_DONE) {
                /* Willy has made it through The Final Barrier without cheating. */
                T(7 + 10 + 10 + 17);
                DrawASprite(16467, WillySpriteData2, 0);
                T(10 + 10 + 17);
                DrawASprite(16563, SwordfishGraphicData, 0);
                M(22611) = 47;       M(22612) = 47;
                M(22611 + 0x20) = 39; M(22612 + 0x20) = 39;
                M(22611 + 0x60) = 69; M(22612 + 0x60) = 69;
                M(22611 + 0x80) = 70; M(22612 + 0x80) = 71;
                M(22611 + 0xA0) = 0;  M(22612 + 0xA0) = 0;
                T(10 + 10 + 10 * 10 + 6 * 5 + 11 * 6 + 10 + 7 + 4);
                /* The celebratory sound effect. */
                {
                    uint8_t c = 0, out = 0;
                    for (uint8_t d = 50; d; d--) {
                        do {
                            out254(out);
                            out ^= 24;
                            uint8_t b = (uint8_t)(c + d + d + d);
                            T(7 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + djnz_cost(b) + 4);
                            c--;
                            T(c ? 12 : 7);
                        } while (c);
                        T(4 + 12);
                    }
                }
            }
        }
        /* MoveToTheNextCavern3 */
        a = 0;
        T(4);
    }
    /* MoveToTheNextCavern4 */
    M(CurrentCavernNumber) = a;
    T(13 + 7);
    /* Cycle the INK and PAPER colours of the current cavern. */
    for (a = 0x3F; a; a--) {
        T(10 + 10 + 10);
        fill(0x5800, a, 511);
        T(10);
        for (uint8_t c = 4; c; c--)
            T(djnz_cost(0) + 4 + 12);
        T(-5 + 4 + 12);
    }
    T(13 + 4 + 10);
    if (M(GameModeIndicator)) goto Start7;

    /* Increase the score and decrease the air supply until it runs out. */
    for (;;) {
        T(17 + 10);
        if (DecreaseAirRemaining()) goto Start7;
        T(10 + 17);
        AddToTheScore1(Score2 + 8);
        T(14 + 7 + 10 + 17);
        PrintAMessage(Score2 + 3, 20602, 6);
        uint8_t d = rotl((uint8_t)((~M(RemainingAirSupply)) & 0x3F), 1);
        T(7 + 13 + 4 + 7 + 8 + 4);
        for (uint8_t c = 4; c; c--) {
            T(7);
            out254(0);
            T(4 + djnz_cost(d) + 7);
            out254(24);
            T(4 + djnz_cost(d) + 4 + 12);
        }
        T(-5 + 12);
    }
}
