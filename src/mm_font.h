/*
 * mm_font.h - install the character set at 0x3D00.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MM_FONT_H
#define MM_FONT_H

/* rom16k: a 16384-byte Spectrum ROM image, or NULL for the built-in font. */
void mm_install_font(const unsigned char *rom16k);

#endif
