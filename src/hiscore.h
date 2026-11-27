/* Persistent high scores in the Xbox save area.
 *
 * Title ID 4D4D0058, shared with the nxdk builds, so they share one save
 * folder (U:\ on RXDK is E:\UDATA\4D4D0058\):
 *
 *   U:\TitleMeta.xbx                 "TitleName=Manic Miner"
 *   U:\000000000048\SaveMeta.xbx     "Name=Manic Miner high score"
 *   U:\000000000048\HISCORE.DAT
 *   U:\000000000128\...              (Manic Miner 128)
 *
 * Nothing is written until there is a score to save.
 * SPDX-License-Identifier: MIT */
#ifndef HISCORE_H
#define HISCORE_H

#include <stdint.h>

#define HS_TITLE_ID "4D4D0058"

/* Nothing to mount on RXDK (XAPI maps U:\). Safe to call more than once. */
void hs_init(void);

/* Load the saved high score for one game (0 = 48K, 1 = 128) as six ASCII
 * digits. Returns 1 if a valid save was found. */
int hs_load(int v128, uint8_t digits[6]);

/* Save it. Returns 1 on success. Writes HISCORE.TMP, deletes HISCORE.DAT
 * and renames, so a power cut leaves at least one valid copy. */
int hs_save(int v128, const uint8_t digits[6]);

#endif
