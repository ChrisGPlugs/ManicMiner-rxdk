/*
 * mm_mem.h - Manic Miner's original memory map.
 *
 * Every address here is the address the label has in the original 48K
 * Spectrum game (see ManicMiner.asm in WHumphreys/Manic-Miner-Source-Code,
 * which assembles byte-for-byte to ManicMiner.bin). The port loads
 * ManicMiner.bin into mem[] unchanged and runs the translated logic against
 * these same addresses.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MM_MEM_H
#define MM_MEM_H

/* ---- Spectrum hardware ------------------------------------------------- */
#define ROM_FONT                 0x3D00   /* character set used by PrintASingleCharacter */
#define DISPLAY_FILE             0x4000
#define ATTRIBUTE_FILE           0x5800

/* ---- Buffers (0x5C00 - 0x7FFF) ----------------------------------------- */
#define AttributeBufferCWGI      0x5C00   /* cavern + Willy + guardians + items */
#define EmptyCavernAttributeBuffer 0x5E00
#define ScreenBufferCWGI         0x6000
#define EmptyCavernScreenBuffer  0x7000

/* ---- Current cavern definition (0x8000 - 0x83FF) ----------------------- */
#define CavernName               0x8000
#define BackgroundTile           0x8020
#define FloorTile                0x8029
#define CrumblingFloorTile       0x8032
#define WallTile                 0x803B
#define ConveyorTile             0x8044
#define NastyTile1               0x804D
#define NastyTile2               0x8056
#define ExtraTile                0x805F
#define WillysPixelYCoord        0x8068
#define WillysAnimationFrame     0x8069
#define WillysDirAndMovFlags     0x806A
#define AirborneStatusIndicator  0x806B
#define WillysLocInAttrBuffer    0x806C   /* word */
#define JumpingAnimationCounter  0x806E
#define ConveyorDirection        0x806F
#define ConveyorAddress          0x8070   /* word */
#define ConveyorLength           0x8072
#define BorderColor              0x8073
#define AttrLastItemDrawn        0x8074
#define ItemDef1                 0x8075   /* 5 x 5 bytes + terminator at 0x808E */
#define PortalDefAttributeByte   0x808F
#define PortalDefGraphicData     0x8090
#define PortalDefAttributeBuf    0x80B0   /* word */
#define PortalDefScreenBuf       0x80B2   /* word */
#define ItemGraphic              0x80B4
#define RemainingAirSupply       0x80BC
#define GameClock                0x80BD
#define HorizontalGuardian1      0x80BE   /* 4 x 7 bytes + terminator at 0x80DA */
#define HorizontalGuardian2      0x80C5
#define EugDirOrKongBeastStatus  0x80DB
#define MultiUseCoordinateStore  0x80DC
#define VerticalGuardian1        0x80DD   /* 4 x 7 bytes + terminator at 0x80F9 */
#define EugeneGraphicData        0x80E0   /* used only in Eugene's Lair */
#define GuardianGraphicData      0x8100

/* ---- Fixed game data and variables (0x8200 - 0x85CB) ------------------- */
#define WillySpriteData          0x8200
#define WillySpriteData1         0x8240
#define WillySpriteData2         0x8260
#define YTable                   0x8300   /* screen buffer address lookup */
#define AppEntry                 0x8400
#define CurrentCavernNumber      0x8407
#define WillyNotMoving0          0x8408   /* left-right movement table */
#define AirText                  0x8418
#define HighScore                0x841F
#define Score1                   0x8425
#define Score2                   0x8426
#define HighScoreText            0x842F
#define GameText                 0x844F
#define OverText                 0x8453
#define LivesRemaining           0x8457
#define ScreenFlashCounter       0x8458
#define KempJoystickIndicator    0x8459
#define GameModeIndicator        0x845A
#define MusicNoteIndex           0x845B
#define MusicFlags               0x845C
#define KeyCounter               0x845D
#define KP6031769xN              0x845E
#define KP6031769x6              0x8460
#ifndef MM128
#define TitleScreenTuneData      0x846E
#define InGameTuneData           0x858C
#define Start_CODE               0x85CC   /* first byte of the 'Start' routine */
#else
/* Software Projects / 128: the cheat table is 6 bytes longer (TYPEWRITER),
 * so everything from the title tune on moves up by 6. */
#define TitleScreenTuneData      33908
#define InGameTuneData           34194    /* still present, no longer played */
#define Start_CODE               34258
#endif
#define MemTop                   0x9CFE   /* original stack top */

#ifndef MM128
/* ---- Title screen and cavern data (0x9D00 - 0xFFFF) -------------------- */
#define TitleScreenBanner        0x9D00
#define BottomAttributes         0x9E00
#define TitleScreenDataTop       0xA000
#define CentralCavernData        0xB000   /* 20 caverns x 1024 bytes */
#define SwordfishGraphicData     0xB2E0
#define TheMenagerieData         0xB800
#define BootGraphicData          0xBAE0   /* = TheMenagerieData + 736 */
#define TheFinalBarrierData      0xFC00

#define PlinthGraphicData        0xB6E0   /* in The Cold Room's spare bytes */
#define DCCTSB4_OPERAND          0x8A9C   /* operand of the self-modified LD D,n */

#else
/* ---- Manic Miner 128 (Mihai Novitchi / Bob Fossil, Software Projects) --- */
#define DCCTSB4_OPERAND          35495
#define SwordfishGraphicData     37739
#define PlinthGraphicData        37771
#define BootGraphicData          37803
#define Unpack                   37835    /* LZ unpacker; self-modifying: */
#define UnpackMarker             37956    /*   operand of CP n (escape byte) */
#define UnpackHookDE             37962    /*   RET (201) or NOP (0): DE 0x6000 -> 0x8000 */
#define UnpackHookHL             37974    /*   RET (201) or NOP (0): HL 0x6000 -> 0x8000 */
#define MenuText                 37986    /* "1 Infinite lives: NO ..." */
#define MenuLivesYesNo           38004
#define MenuAirYesNo             38023
#define MenuMusicYesNo           38035
#define MenuMusicOffJump         38328    /* operand of JP nn (-> 40016) */
#define TitleTuneCall            34360    /* operand of CALL nn (-> 40023) */
#define PatchAirCall             34843    /* CALL DecreaseAirRemaining / LD A,1:OR A */
#define PatchLivesDec            35147    /* DEC (HL) (53) / NOP (0) */
#define PatchBeamCalls           36264    /* 4 x CALL DecreaseAirRemaining / NOPs */
#define DecreaseAirRemaining_CODE 35399
#define IM2Table                 39680    /* 257 bytes of 156 -> vector 0x9C9C */
#define IM2Handler               40092
#define MusicOn                  40113    /* operand of LD BC,nn: 1 = music on */
#define PackedTitleScreen        40192    /* 4096 bytes when unpacked */
#define TitleScreenBanner        41934
#define PackedTitleAttributes    42190    /* 768 bytes when unpacked */
#define CavernTable              42568    /* 20 x (word address, byte) */
#define STPlayerInit             49152
#define STPlayerPlay             49158
#endif

#endif
