/*
 * st_player.h - Sound Tracker player from Manic Miner 128 (translated).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ST_PLAYER_H
#define ST_PLAYER_H

#include <stdint.h>

void st_init(void);   /* 49152: start the tune from the beginning (DI ... EI) */
void st_play(void);   /* 49158: one 50 Hz tick, called from the IM2 handler */

/* The player reads register C on entry in one path (channel A's mixer bits
 * when channel A is silent); the interrupted code's C is unknowable here, so
 * the caller may set it (the test harness feeds it the reference value). */
void st_set_entry_c(uint8_t c);

#endif
