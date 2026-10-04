# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- A scripted new game reaches the Endar Spire. It runs character generation (Scoundrel, Quick
  Character, portrait, random name, Play), the area load and the opening conversation, all in
  lifted code. The conformance harness runs it: 11/11 milestones.
- `--click x,y@s`: a move, press and release posted to the game's window, timed from the main
  menu (its music opening), not from process start.
- Milestone lines for the game's own media and area loads: Bink movies, Miles streams, module
  archives, and the first frame after a module opens. Documented in `docs/testing.md`.
- The recompiled game boots through the intro movies to the main menu (6/6 boot milestones).
- `--original`: the unwrapped exe's own machine code under the same host and shims, the
  reference a lifted run is compared against (oracle.c, from Red Alert 2's host).
- `--headless --record out.mp4`: frames read back from the GL back buffer and piped to
  ffmpeg, so a run shows what it drew with no display.

### Fixed
- The script VM no longer corrupts esi on its first engine command. The command table is filled
  by 7 KB of straight-line code that disasm32 cut at 4 KB, so 142 command functions were missing
  from the catalog and their dispatches came back without popping their arguments. Fixed in
  disasm32 (pcrecomp #50); the catalog is now 29,548 functions.
- DirectInput is created. `DirectInput8Create` refuses an HINSTANCE Windows didn't load, and
  the game passes the guest image's base. With no input object, its input manager read through
  null at 0x005E2F93 and 0x005E4053, in both the lift and the original. The host's instance
  stands in for it.
- The watchdog flushes stdout, so the milestone lines a killed run printed reach the harness.
- Survey of the Steam build (`docs/RECON.md`). It covers `swkotor.exe` 1.03, its SteamStub
  2.x wrapper layer by layer, the game data, the community material and the licence of each
  item, and the engine-level fix list.
- The Steam wrapper comes off with pcrecomp's `tools/drm/steamstub.py`, added for this game
  (pcrecomp #46): statically, with no Steam, in under a second.
- Pipeline: `run_lift.py` (lift32 over the `disasm32` catalog, with vtable-scan seeds since
  KotOR has no RTTI), `CMakeLists.txt` and `build.cmd` for the 32-bit native32 host, and
  `Setup.cmd` / `tools/setup.ps1` (Quick start: links the install as `game\`, unwraps,
  catalogs, lifts, builds).
- Host (`src/runtime/host.c`):
  - Muted by default (the process audio session), and the run stops if muting fails.
  - `--headless`.
  - `ChangeDisplaySettingsA` and `SetDeviceGammaRamp` answered without touching the
    real displays.
  - Milestone lines, fault report, watchdog.
- Needs pcrecomp #48 and #49 (native32): the bridge kept clobbering clang-cl's base register,
  and it lost the guest's last error, which hung the resource scan.
- Conformance harness (`tools/conformance.py`, `conformance.json`): boot milestones and
  lift health, failing on regression.
