/*
 * platform.h - the console side of the Manic Miner RXDK port.
 *
 * The game and the Spectrum model (zx.c, mm_game.c, ...) are plain C and
 * know nothing about the Xbox. main.c is the front end (audio, input,
 * pacing, the chooser); this layer is everything that needs the Xbox's own
 * APIs: the video mode and Direct3D presentation, the debug log, the save
 * area's file operations, the in-game reset and leaving to the dashboard.
 *
 * Implementation: platform_rxdk.cpp (RXDK-SDL3 + Direct3D 8).
 * SPDX-License-Identifier: MIT
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pick a video mode and open the window / Direct3D device. The picture is
 * a src_w x src_h XRGB8888 frame shown at 4:3, centred. Returns 0 on failure. */
int  plat_video_init(int src_w, int src_h);

/* Show a frame (src_w x src_h, pitch = src_w pixels). Waits for vblank. */
void plat_present(const uint32_t *pixels);

/* Description of the mode in use, e.g. "1280x720 (picture 960x720 3x)". */
const char *plat_video_mode_name(void);

/* Wall-clock microseconds. */
uint64_t plat_now_us(void);

/* printf-style line to the debug output (serial in xemu). */
void plat_log(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);

/* Folders. Both end in a backslash. */
const char *plat_data_dir(void);     /* D:\ : next to default.xbe           */
const char *plat_save_dir(void);     /* U:\ : E:\UDATA\<title id>\          */

/* Save-area file operations (return 1 on success). */
int  plat_make_dir(const char *path);
int  plat_delete_file(const char *path);
int  plat_rename_file(const char *from, const char *to);
/* Make sure a file just written and closed is on the disk (fclose leaves
 * it in the FATX cache, where a power cut loses it). */
void plat_commit_file(const char *path);

/* Call back fn(path, user) for every file in dir (dir ends in '\'). */
void plat_list_dir(const char *dir, void (*fn)(const char *path, void *user), void *user);

/* In-game reset: call once per frame with the open gamepad (may be NULL).
 * LT + RT + Back + Start, or LT + RT + Back + Black, reboots to the
 * dashboard and doesn't return. */
void plat_check_igr(SDL_Gamepad *pad);

/* Leave to the dashboard. */
void plat_exit(void) SDL_NORETURN;

#ifdef __cplusplus
}
#endif

#endif
