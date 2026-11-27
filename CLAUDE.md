# manicminer-rxdk (Manic Miner on RXDK + SDL3)

Follow the workspace rules in `../../CLAUDE.md`. Port-specific notes:

- `../../ref/ManicMiner-nxdk` is the read-only nxdk reference (desktop build, difftests). Don't edit it.
- Game code is the nxdk port's, unchanged: `zx.c`, `mm_game.c` (built twice via `mm_game48.c` /
  `mm_game128.c`, the latter with `-DMM128`), `mm_font.c`, `ay.c`, `st_player.c`, `mm128_load.c`.
  Keep it in sync with nxdk rather than changing it here.
- Port layer: `main.c` (front end: audio stream, gamepad, pacing, chooser), `platform_rxdk.cpp`
  (video/D3D8, log, save-file ops, IGR, exit), `hiscore.c` (U:\ saves, same format as nxdk).
- **Game data is never committed.** `datafiles/` is git-ignored apart from its README; the build
  deploys it to `D:\datafiles\`. Delete `out/Release` after changing it (not tracked as an input).
- Test: `pwsh -File tools/rxdk-run.ps1 -Project ports/manicminer-rxdk -Seconds 75 -Until 'MM READY'`.
  To test one game alone, move the other data file out of `datafiles/` and delete `out/Release`.
  `MM STATS` lines: fps ~50, audio ~48000, gaps 0; contention ~17% in caverns.
- Title ID 0x4D4D0058 (`imageBuild.testId`), shared with the nxdk XBEs.
