/*
 * mm128_load.h - build Manic Miner 128's 64K memory image from its TAP.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MM128_LOAD_H
#define MM128_LOAD_H

#include <stdint.h>

enum { MM128_OK = 0, MM128_ERR_TAP = -1, MM128_ERR_VERSION = -2 };

/* image: 65536 bytes, filled with the memory contents at the moment the
 * tape's loader jumps to AppEntry (33792). */
int mm128_build_image(const uint8_t *tap, long tap_len, uint8_t *image);

#endif
