/*
 * zx.c - memory, clock, ports, beeper and screen for the Manic Miner port.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "zx.h"

uint8_t  mem[65536];
uint64_t zx_t;
uint32_t zx_cpu_hz = 3500000;
uint32_t zx_t_per_frame = 69888;
uint32_t zx_contend_start = 14335;
uint32_t zx_t_per_line = 224;
uint64_t zx_next_frame = 69888;
int      zx_contention = 1;
uint64_t zx_contended_t;
uint32_t zx_contend_frac;

void zx_set_model(zx_model_t model)
{
    if (model == ZX_MODEL_128K) {
        zx_cpu_hz = 3546900; zx_t_per_frame = 70908; zx_contend_start = 14361; zx_t_per_line = 228;
    } else {
        zx_cpu_hz = 3500000; zx_t_per_frame = 69888; zx_contend_start = 14335; zx_t_per_line = 224;
    }
    zx_next_frame = zx_t + zx_t_per_frame;
}

uint8_t zx_keys[8];
uint8_t zx_kempston;
int     zx_kempston_present = 1;
uint8_t zx_border;

zx_beep_t zx_beeps[ZX_BEEPER_MAX];
int       zx_beep_count;
uint8_t   zx_beep_level;

void (*zx_frame_hook)(void);
/* Test hooks used by mm_game.c (shared by both engines in the combined build). */
void (*mm_mainloop_hook)(void);
void (*mm_music_tick_hook)(void);
void (*mm_game_over_hook)(void);
int mm_in_title;
/* Re-entrancy guard for zx_frame (file scope so zx_reset can clear it if
 * the host leaves the game from inside a frame hook with longjmp). */
static int in_frame;
int  zx_iff, zx_im;
void (*zx_int_hook)(void);
zx_ay_t zx_ay_log[ZX_AY_MAX];
int     zx_ay_count;
uint8_t zx_ay_regs[16];

void zx_keys_clear(void)
{
    memset(zx_keys, 0x1F, sizeof zx_keys);
}

void zx_reset(void)
{
    zx_t = 0;
    zx_next_frame = ZX_T_PER_FRAME;
    zx_contended_t = 0;
    zx_keys_clear();
    zx_kempston = 0;
    zx_border = 0;
    zx_beep_count = 0;
    zx_beep_level = 0;
    zx_iff = 0;
    zx_im = 1;
    zx_int_hook = 0;
    in_frame = 0;
    zx_ay_count = 0;
    memset(zx_ay_regs, 0, sizeof zx_ay_regs);
}

void zx_ay_write(uint8_t reg, uint8_t val)
{
    reg &= 15;
    zx_ay_regs[reg] = val;
    if (zx_ay_count < ZX_AY_MAX) {
        uint64_t frame_start = zx_next_frame - ZX_T_PER_FRAME;
        zx_ay_log[zx_ay_count].t = (uint32_t)(zx_t - frame_start);
        zx_ay_log[zx_ay_count].reg = reg;
        zx_ay_log[zx_ay_count].val = val;
        zx_ay_count++;
    }
}

void zx_key(int row, int bit, int down)
{
    if (down) zx_keys[row] &= (uint8_t)~(1u << bit);
    else      zx_keys[row] |= (uint8_t)(1u << bit);
}

void zx_frame(void)
{
    if (in_frame) return;            /* the interrupt handler ticks the clock too */
    in_frame = 1;
    while (zx_t >= zx_next_frame) {
        if (zx_frame_hook)
            zx_frame_hook();
        /* The hook consumed this frame's beeper and AY events. */
        zx_beep_count = 0;
        zx_ay_count = 0;
        zx_next_frame += ZX_T_PER_FRAME;
        /* New frame: the ULA raises /INT. */
        if (zx_int_hook && zx_iff)
            zx_int_hook();
    }
    in_frame = 0;
}

uint8_t (*zx_in_filter)(uint16_t port, uint8_t value);

static uint8_t zx_in_raw(uint16_t port);

uint8_t zx_in(uint16_t port)
{
    uint8_t v = zx_in_raw(port);
    return zx_in_filter ? zx_in_filter(port, v) : v;
}

static uint8_t zx_in_raw(uint16_t port)
{
    /* Kempston interface: port 0x1F (only A5 decoded on the real thing,
     * but the game always uses 0x1F). Bits 5-7 read as 0 when present. */
    if ((port & 0xFF) == 0x1F) {
        return zx_kempston_present ? (uint8_t)(zx_kempston & 0x1F) : 0xFF;
    }
    if (!(port & 1)) {
        /* ULA: every half-row whose address line is low is ANDed in. */
        uint8_t v = 0x1F;
        uint8_t hi = (uint8_t)(port >> 8);
        for (int row = 0; row < 8; row++)
            if (!(hi & (1u << row)))
                v &= zx_keys[row];
        return (uint8_t)(v | 0xA0);   /* bit 6 (EAR) low, bits 5 & 7 high */
    }
    return 0xFF;
}

void zx_out(uint16_t port, uint8_t value)
{
    if (!(port & 1)) {
        zx_border = value & 7;
        uint8_t level = (uint8_t)(value & 0x18);   /* EAR (bit 4) + MIC (bit 3) */
        if (level != zx_beep_level) {
            zx_beep_level = level;
            if (zx_beep_count < ZX_BEEPER_MAX) {
                uint64_t frame_start = zx_next_frame - ZX_T_PER_FRAME;
                zx_beeps[zx_beep_count].t = (uint32_t)(zx_t - frame_start);
                zx_beeps[zx_beep_count].level = level;
                zx_beep_count++;
            }
        }
    }
}

/* Spectrum palette (normal, bright). */
static const uint32_t zx_palette[16] = {
    0xFF000000, 0xFF0000D7, 0xFFD70000, 0xFFD700D7,
    0xFF00D700, 0xFF00D7D7, 0xFFD7D700, 0xFFD7D7D7,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};

void zx_render(uint32_t *pixels, int pitch, int flash_phase)
{
    const uint32_t border = zx_palette[zx_border & 7];
    const int bx = (ZX_SCREEN_W - 256) / 2, by = (ZX_SCREEN_H - 192) / 2;

    for (int y = 0; y < ZX_SCREEN_H; y++) {
        uint32_t *row = pixels + y * pitch;
        int sy = y - by;
        if (sy < 0 || sy >= 192) {
            for (int x = 0; x < ZX_SCREEN_W; x++) row[x] = border;
            continue;
        }
        for (int x = 0; x < bx; x++) row[x] = border;
        for (int x = bx + 256; x < ZX_SCREEN_W; x++) row[x] = border;

        /* Display file address for pixel row sy. */
        uint16_t line = (uint16_t)(0x4000 | ((sy & 0xC0) << 5) | ((sy & 7) << 8) | ((sy & 0x38) << 2));
        uint16_t attr_line = (uint16_t)(0x5800 + (sy >> 3) * 32);
        uint32_t *out = row + bx;
        for (int cx = 0; cx < 32; cx++) {
            uint8_t bits = mem[line + cx];
            uint8_t attr = mem[attr_line + cx];
            int bright = (attr & 0x40) ? 8 : 0;
            uint32_t ink = zx_palette[(attr & 7) + bright];
            uint32_t paper = zx_palette[((attr >> 3) & 7) + bright];
            if ((attr & 0x80) && flash_phase) { uint32_t t = ink; ink = paper; paper = t; }
            for (int b = 0; b < 8; b++)
                *out++ = (bits & (0x80 >> b)) ? ink : paper;
        }
    }
}
