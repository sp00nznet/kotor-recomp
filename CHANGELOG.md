# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
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
  lift health, failing on regression. Baseline 3/6 milestones, 0 lift errors.
