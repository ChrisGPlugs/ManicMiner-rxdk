/*
 * zx.h - the bits of a 48K ZX Spectrum that the Manic Miner port needs.
 *
 * The game code (mm_game.c) works directly on a 64K memory array laid out
 * exactly like the Spectrum's: ROM at 0x0000, display file at 0x4000,
 * attributes at 0x5800, and the game itself from 0x5C00 upwards. It talks to
 * the "hardware" through zx_in()/zx_out() and advances a Z80 T-state clock
 * with zx_tick(), so the original timing (and the beeper music) is preserved.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZX_H
#define ZX_H

#include <stdint.h>

/* Machine timing (set by zx_set_model). */
typedef enum { ZX_MODEL_48K, ZX_MODEL_128K } zx_model_t;
extern uint32_t zx_cpu_hz;               /* 3500000 (48K) / 3546900 (128K) */
extern uint32_t zx_t_per_frame;          /* 69888 (48K) / 70908 (128K) */
extern uint32_t zx_contend_start;        /* first contended T-state of a frame */
extern uint32_t zx_t_per_line;           /* 224 (48K) / 228 (128K) */
#define ZX_CPU_HZ      zx_cpu_hz
#define ZX_T_PER_FRAME zx_t_per_frame
void zx_set_model(zx_model_t model);

/* Full 64K address space, original memory map. */
extern uint8_t mem[65536];

/* T-state clock. */
extern uint64_t zx_t;
extern uint64_t zx_next_frame;
void zx_frame(void);                     /* called every ZX_T_PER_FRAME T-states */

/* Signed so the translation can express "JR not taken" style corrections. */
static inline void zx_tick(int32_t t)
{
    zx_t = (uint64_t)((int64_t)zx_t + t);
    if (zx_t >= zx_next_frame)
        zx_frame();
}

/* ULA memory contention. While the ULA fetches the screen (the first 128
 * T-states of each of the 192 display lines), a CPU access to
 * 0x4000-0x7FFF waits 6,5,4,3,2,1,0,0 T-states depending on where in the
 * 8-T fetch cycle it lands. Manic Miner keeps its screen and attribute
 * buffers in that region, so this slows the real game by about a quarter.
 * The comparison tests switch it off (zx_contention = 0) because the
 * reference Z80 core doesn't model it. */
extern int      zx_contention;
extern uint64_t zx_contended_t;          /* total delay added (diagnostics) */
static inline void zx_contend_now(void)
{
    uint32_t ft = (uint32_t)(zx_t - (zx_next_frame - zx_t_per_frame));
    if (ft < zx_contend_start) return;
    uint32_t d = ft - zx_contend_start;
    uint32_t line = d / zx_t_per_line;
    if (line >= 192) return;
    uint32_t x = d - line * zx_t_per_line;
    if (x >= 128) return;
    static const uint8_t pattern[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    uint8_t delay = pattern[x & 7];
    if (delay) { zx_contended_t += delay; zx_tick(delay); }
}
/* Exact contention at the current T-state: used where the translation
 * advances the clock cycle by cycle (LDIR, port I/O). */
static inline void zx_contend(uint16_t addr)
{
    if (zx_contention && (addr & 0xC000) == 0x4000) zx_contend_now();
}
/* Expected contention: the translated routines make their memory accesses
 * in bursts and charge the instruction T-states afterwards, so the exact
 * pattern would let every access after the first in a burst through for
 * free. Instead, an access made while the ULA is fetching the screen costs
 * the pattern's average, 21/8 T-states, carried as a fraction. */
extern uint32_t zx_contend_frac;
static inline void zx_contend_avg(uint16_t addr)
{
    if (!zx_contention || (addr & 0xC000) != 0x4000) return;
    uint32_t ft = (uint32_t)(zx_t - (zx_next_frame - zx_t_per_frame));
    if (ft < zx_contend_start) return;
    uint32_t d = ft - zx_contend_start;
    uint32_t line = d / zx_t_per_line;
    if (line >= 192 || d - line * zx_t_per_line >= 128) return;
    zx_contend_frac += 21;
    uint32_t delay = zx_contend_frac >> 3;
    zx_contend_frac &= 7;
    if (delay) { zx_contended_t += delay; zx_tick((int32_t)delay); }
}
/* A data access to memory, as made by the game code: contention, then the
 * byte itself. */
static inline uint8_t *zx_mem(uint16_t addr)
{
    zx_contend_avg(addr);
    return &mem[addr];
}
/* I/O to the ULA (even port): contended like a memory access. */
static inline void zx_contend_io(uint16_t port)
{
    if (zx_contention && (!(port & 1) || (port & 0xC000) == 0x4000)) zx_contend_now();
}

/* Port I/O, as seen by the game. */
uint8_t zx_in(uint16_t port);
/* Optional filter on every IN (used by the comparison test). */
extern uint8_t (*zx_in_filter)(uint16_t port, uint8_t value);
void    zx_out(uint16_t port, uint8_t value);

/* Keyboard matrix: 8 half-rows, bits 0-4, 0 = pressed (like the real ULA). */
extern uint8_t zx_keys[8];
/* Kempston joystick: bit0 right, 1 left, 2 down, 3 up, 4 fire (1 = active). */
extern uint8_t zx_kempston;
extern int     zx_kempston_present;

/* Half-row indices for zx_keys[] (address line A8..A15 that selects them). */
enum {
    ZX_ROW_SHIFT_V = 0,   /* 0xFEFE: SHIFT Z X C V   */
    ZX_ROW_A_G     = 1,   /* 0xFDFE: A S D F G       */
    ZX_ROW_Q_T     = 2,   /* 0xFBFE: Q W E R T       */
    ZX_ROW_1_5     = 3,   /* 0xF7FE: 1 2 3 4 5       */
    ZX_ROW_0_6     = 4,   /* 0xEFFE: 0 9 8 7 6       */
    ZX_ROW_P_Y     = 5,   /* 0xDFFE: P O I U Y       */
    ZX_ROW_ENTER_H = 6,   /* 0xBFFE: ENTER L K J H   */
    ZX_ROW_SPACE_B = 7    /* 0x7FFE: SPACE SYM M N B */
};
void zx_key(int row, int bit, int down);
void zx_keys_clear(void);

/* Border colour (last value written to port 0xFE, bits 0-2). */
extern uint8_t zx_border;

/* Beeper: speaker level changes recorded with T-state offsets within the
 * current frame. The host turns them into PCM in its frame hook. */
#define ZX_BEEPER_MAX 32768
typedef struct { uint32_t t; uint8_t level; } zx_beep_t;
extern zx_beep_t zx_beeps[ZX_BEEPER_MAX];
extern int       zx_beep_count;
extern uint8_t   zx_beep_level;          /* level at start of frame / now */

/* Render the display file + border into a 320x240 ARGB8888 buffer
 * (32-pixel border left/right, 24-pixel border top/bottom). flash_phase
 * selects the inverted half of the FLASH cycle. */
#define ZX_SCREEN_W 320
#define ZX_SCREEN_H 240
void zx_render(uint32_t *pixels, int pitch_pixels, int flash_phase);

/* Hook the host installs: called once per emulated frame (50 Hz), at the
 * end of the frame (beeper/AY events for the frame are complete). */
extern void (*zx_frame_hook)(void);

/* Maskable interrupt, raised at the start of every frame. The game code
 * keeps zx_iff/zx_im up to date at its DI/EI/IM instructions; the game
 * installs zx_int_hook to run its interrupt handler (Manic Miner 128's IM2
 * music player). The original Manic Miner runs with interrupts off. */
extern int  zx_iff, zx_im;
extern void (*zx_int_hook)(void);

/* AY-3-8912 (128K sound chip): register writes recorded with T-state
 * offsets within the current frame, plus the current register file. */
#define ZX_AY_MAX 4096
typedef struct { uint32_t t; uint8_t reg, val; } zx_ay_t;
extern zx_ay_t zx_ay_log[ZX_AY_MAX];
extern int     zx_ay_count;
extern uint8_t zx_ay_regs[16];
void zx_ay_write(uint8_t reg, uint8_t val);

void zx_reset(void);

#endif
