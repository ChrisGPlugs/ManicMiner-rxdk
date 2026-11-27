/*
 * ay.h - AY-3-8912 sound chip emulation for the front end.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AY_H
#define AY_H

#include <stdint.h>

#define AY_CLOCK_128K 1773400.0

typedef struct {
    uint8_t r[16];
    unsigned tone_count[3];
    int tone_out[3];
    unsigned noise_count;
    uint32_t rng;
    int noise_out;
    unsigned env_tick;
    int env_count, env_attack, env_hold, env_alt, env_holding;
    double ticks_per_sample, tick_acc;
} ay_t;

void  ay_init(ay_t *ay, double clock_hz, int sample_rate);
void  ay_write(ay_t *ay, int reg, uint8_t v);
float ay_sample(ay_t *ay);      /* 0..1, mono mix of the three channels */

#endif
