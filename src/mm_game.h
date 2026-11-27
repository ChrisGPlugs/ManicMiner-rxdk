/*
 * mm_game.h - entry point of the translated game.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MM_GAME_H
#define MM_GAME_H

/* Run the game from AppEntry (33792). Never returns; the host gets control
 * back every emulated frame through zx_frame_hook. mm_game.c builds as
 * mm_run48 (Bug-Byte) or, with -DMM128, as mm_run128 (Manic Miner 128); the
 * combined build links both. */
void mm_run48(void);
void mm_run128(void);
#ifdef MM128
#define mm_run mm_run128
#else
#define mm_run mm_run48
#endif

/* Optional hook called at the top of every pass through the main loop
 * (address 0x870E in the original). Used by the comparison test. The hooks
 * are defined in zx.c, so both engines can share them. */
extern void (*mm_mainloop_hook)(void);

/* Set while the game is on its title screen (from Start until a game or
 * the demo begins); lets the combined build's front end offer a way back to
 * its chooser. */
extern int mm_in_title;

/* Optional hook called after each AY music tick (MM128 only; test use). */
extern void (*mm_music_tick_hook)(void);

/* Optional hook called when a game ends (all lives lost), just before the
 * game compares the score (six ASCII digits at Score2 + 3) with HighScore. */
extern void (*mm_game_over_hook)(void);

#endif
