/*
 * ay.c - AY-3-8912 sound chip, as used in the ZX Spectrum 128.
 *
 * Three square-wave tone channels, a 17-bit LFSR noise generator and the
 * hardware envelope, stepped at 1/8 of the chip clock (1.7734 MHz on the
 * Spectrum 128) and box-filtered down to the output sample rate.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "ay.h"

/* Measured AY DAC levels (normalised). */
static const float ay_level[16] = {
    0.0000f, 0.0137f, 0.0205f, 0.0291f, 0.0423f, 0.0618f, 0.0847f, 0.1369f,
    0.1691f, 0.2647f, 0.3527f, 0.4499f, 0.5704f, 0.6873f, 0.8482f, 1.0000f
};

void ay_init(ay_t *ay, double clock_hz, int sample_rate)
{
    memset(ay, 0, sizeof *ay);
    ay->ticks_per_sample = clock_hz / 8.0 / sample_rate;
    ay->rng = 1;
    ay->env_count = 15;
}

void ay_write(ay_t *ay, int reg, uint8_t v)
{
    reg &= 15;
    ay->r[reg] = v;
    if (reg == 13) {
        ay->env_attack = (v & 4) ? 15 : 0;
        if (!(v & 8)) { ay->env_hold = 1; ay->env_alt = ay->env_attack != 0; }
        else { ay->env_hold = v & 1; ay->env_alt = (v & 2) != 0; }
        ay->env_count = 15;
        ay->env_holding = 0;
        ay->env_tick = 0;
    }
}

static void ay_tick(ay_t *ay)
{
    /* Tone: toggle every TP ticks (TP = 12-bit period). */
    for (int ch = 0; ch < 3; ch++) {
        unsigned tp = ay->r[ch * 2] | ((ay->r[ch * 2 + 1] & 15) << 8);
        if (!tp) tp = 1;
        if (++ay->tone_count[ch] >= tp) {
            ay->tone_count[ch] = 0;
            ay->tone_out[ch] ^= 1;
        }
    }
    /* Noise: shift the LFSR every 2 * NP ticks. */
    unsigned np = ay->r[6] & 31;
    if (!np) np = 1;
    if (++ay->noise_count >= np * 2) {
        ay->noise_count = 0;
        ay->rng = (ay->rng >> 1) | ((((ay->rng >> 0) ^ (ay->rng >> 3)) & 1) << 16);
        ay->noise_out = ay->rng & 1;
    }
    /* Envelope: one of 16 steps every 2 * EP ticks. */
    unsigned ep = ay->r[11] | (ay->r[12] << 8);
    if (!ep) ep = 1;
    if (++ay->env_tick >= ep * 2) {
        ay->env_tick = 0;
        if (!ay->env_holding) {
            ay->env_count--;
            if (ay->env_count < 0) {
                if (ay->env_hold) {
                    if (ay->env_alt) ay->env_attack ^= 15;
                    ay->env_holding = 1;
                    ay->env_count = 0;
                } else {
                    if (ay->env_alt) ay->env_attack ^= 15;
                    ay->env_count = 15;
                }
            }
        }
    }
}

float ay_sample(ay_t *ay)
{
    ay->tick_acc += ay->ticks_per_sample;
    int n = (int)ay->tick_acc;
    ay->tick_acc -= n;
    if (n < 1) n = 1;
    float sum = 0.0f;
    uint8_t mixer = ay->r[7];
    for (int i = 0; i < n; i++) {
        ay_tick(ay);
        int env_vol = ay->env_count ^ ay->env_attack;
        for (int ch = 0; ch < 3; ch++) {
            int tone_on  = ay->tone_out[ch] | ((mixer >> ch) & 1);
            int noise_on = ay->noise_out | ((mixer >> (ch + 3)) & 1);
            if (tone_on & noise_on) {
                uint8_t amp = ay->r[8 + ch];
                sum += ay_level[(amp & 0x10) ? env_vol : (amp & 15)];
            }
        }
    }
    return sum / (float)n / 3.0f;
}
