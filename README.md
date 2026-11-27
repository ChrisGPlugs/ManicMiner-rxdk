# Manic Miner for the original Xbox (RXDK)

An RXDK + SDL3 build of [ManicMiner-nxdk](https://github.com/ChrisGPlugs/ManicMiner-nxdk): Matthew Smith's
**Manic Miner** (Bug-Byte, 48K) and **Manic Miner 128** (Mihai Novitchi / Bob Fossil, AY music), translated
routine by routine from the Z80 into C. The game code (`src/mm_game.c`, `zx.c`, `ay.c`, `st_player.c`,
`mm128_load.c`) is the nxdk port's, unchanged. See that repository's README for how the translation was
done and verified.

This builds **ManicMinerX**: one XBE with both games. With one data file present that game starts;
with both, a chooser built from Manic Miner's title screen appears (Back twice on a game's title screen
or demo returns to it).

## Game data (not included)

Manic Miner is © Matthew Smith; the data files aren't distributed. Put them in `datafiles/` (git ignores
everything there) before building, and the build copies them into the ISO as `D:\datafiles\`. On a
console you can also put them next to `default.xbe`; that folder is checked first.

| File | |
|---|---|
| `ManicMiner.bin` | the 65,536-byte memory image from [WHumphreys/Manic-Miner-Source-Code](https://github.com/WHumphreys/Manic-Miner-Source-Code) |
| `ManicMiner128.tap` | "Manic Miner 128 by Mihai Novitchi and Bob Fossil - Software Projects version", renamed (the original name is over FATX's 42 characters) |
| `48.rom` | optional: 16K Spectrum ROM, for the original character set (else a public-domain font) |

The dashboard title and save images (`artwork/TITLEIMAGE.XBX`, `SAVEIMAGE.XBX`) are made from `artwork/titleimage-source.png` (the pixel-art picture from the nxdk repo) with `tools/make-titleimage.ps1`.

**Don't publish an ISO built with data files in `datafiles/`.** Release zips contain only the XBE.

## Building

```
pwsh -File tools/rxdk-run.ps1 -Project ports/manicminer-rxdk -NoRun                       # from the workspace root
pwsh -File tools/rxdk-run.ps1 -Project ports/manicminer-rxdk -Seconds 60 -Until 'MM READY'  # build + boot in xemu
```

Output: `out/Release/ManicMinerX.xbe` (copy it to the console as `default.xbe`) and, for xemu, `out/Release/XISO/ManicMinerX.iso`. After changing what's in
`datafiles/`, delete `out/Release` (the build doesn't notice).

## Controls

| Xbox controller | Action |
|---|---|
| D-pad / left stick ← → | Walk |
| A or B | Jump |
| LT / RT | In play: jump left / right (128, with the options menu open: infinite air / music on-off) |
| Start | Start game (128: also closes the options menu) |
| Y | 48K: in-game music on/off. 128: options menu / infinite lives |
| X | Pause |
| Back | In play: END GAME? (A yes, B no). Twice on a title screen or demo: back to the chooser |
| White | Types the cheat code (6031769 / TYPEWRITER) |
| White / Black, cheat on | Teleport to the previous / next cavern |
| Left stick click | Diagnostics overlay |
| Right stick click | Picture 4:3 / 16:9 (stretched to the full width); remembered in `U:\ASPECT.TXT` |
| LT + RT + Back + Start, or LT + RT + Back + Black | In-game reset: back to the dashboard |

## High scores

Title ID `4D4D0058`, the same as the nxdk builds, so saved high scores are shared with them
(`E:\UDATA\4D4D0058\000000000048\HISCORE.DAT` and `...\000000000128\`). Saved at game over, not when a
cheat was used.

## Differences from the nxdk build

* Audio goes through RXDK's DirectSound (the MCPX APU) via an SDL3 audio stream, not straight into the
  AC97. The game is still paced by its audio queue (~80 ms).
* Video: the 320×240 Spectrum picture (with border) is drawn by Direct3D, at 4:3 or, with the right
  stick click, stretched to the full width of a 16:9 screen. It uses the best mode the dashboard allows:

  | Mode | 4:3 | 16:9 |
  |---|---|---|
  | 720p | 960×720, 3× sharp | 1280×720, 4×3 sharp |
  | 1080i | 1280×960 (4×, small bars top and bottom), sharp | 1920×960, sharp |
  | 480p / 480i, 4:3 TV | 640×480, 2× sharp | same (the screen is 4:3) |
  | 480p / 480i, dashboard set to widescreen | 480×480 anamorphic, filtered | 640×480, sharp |
  | 576i (PAL 50 Hz) | 640×576, filtered | same |

  The chooser names the mode and picture in the top border (e.g. `720p  4:3  960x720  sharp`); when a
  game starts without the chooser it is shown for the first five seconds.

* The debug output prints `MM READY`, `MM SCREEN <name>` and every ~10 s an `MM STATS` line (speed,
  audio rate, queue, gaps, ULA contention).

## Licence

Port code: MIT, see [LICENSE](LICENSE). Credits as in the nxdk repository.
