# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `tools/conformance.py` keeps the run's whole output in `work\conformance.log`.
- Saves from headless runs (and `--private-ini` runs) go to `work\saves\`, never into the
  install: every file call the game makes with a `saves\` path is redirected. They are kept
  between runs, so a test can quicksave once and later runs can load it.
- Repeating script times: `--key 1@a60..330/6` presses 1 every 6 s from 60 s to 330 s, and the
  same for `--click`. The script holds up to 1024 events.
- Headless runs tell the game's window it is active and focused (`WM_ACTIVATEAPP`, `WM_SETFOCUS`),
  as Windows would tell a shown, focused window; the game keys its world input off them.
- An `ExitProcess` shim prints the code and closes the recording when the game ends itself.
- Gamepad support (XInput), which KotOR on PC never had. The left stick moves and strafes,
  the triggers turn the camera, the right stick moves the cursor, and A clicks. B and Start send
  Escape; X, Y, LB, RB, the D-pad and Back send R, Space, Q, E, 1-4 and Tab: the game's own
  default keys (`keymap.2da`). It is on when the window is shown and off for `--headless`
  unless `--gamepad`. `docs/gamepad.md`; `kotor.exe --selftest-input` checks the mapping.
- Scripted keys: `--key NAME@s[:hold_ms]`. The keyboard device's `GetDeviceData` is hooked,
  so injected keys reach the game even with its window hidden or unfocused, where DirectInput
  gives it none. Times may be `a<seconds>`, counted from the first frame after an area loads.
- `tools/keymap.py`: the game's key bindings, read from `keymap.2da` in your install.
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
- Headless runs put a black, topmost area on the player's desktop. KotOR's cinematic
  backdrop (`0x00401B37`) creates its window from a class it never registers, so creation
  fails, and then paints `GetDC(NULL)` (the whole screen) black, 1600x1200 from the
  top-left. The game's `SetWindowPos` also raised and activated the hidden window.
  Headless now refuses the screen DC and never shows, raises or activates a window, and
  the game no longer moves the real cursor.
- Headless input no longer depends on the desktop. Activation was posted to every new game
  window; the game answers it by resetting its display, which makes a new window, so at the
  main menu it looped about once a second and clicks landed on windows about to go. It is
  posted once. The shipping code (`--original`) now takes the scripted New Game click every
  time (8 of 8, hidden), where before it took it only when Windows let its window take the
  foreground.
- The script's clock starts at the first frame after the menu music opens, not at the
  music: under `--original` the music opens over 12 s before anything is drawn, and clicks
  went to no window.
- The LICENSE note on what the MIT grant doesn't cover named the wrong game. It now names
  KotOR and says `game\` is a junction to the install, not a copy.
- Long recorded runs no longer die at their watchdog with `0xC0000409` and no report. The
  watchdog closed the ffmpeg pipe from its own thread while the render thread was writing to it,
  and the C runtime failed fast. The pipe is now locked, and a closed recording stays closed.
- `TerminateProcess` on the game's own process is logged with its caller, like `ExitProcess`.
- Test runs no longer change the player's settings. KotOR rewrites `swkotor.ini` by itself, and
  scripted runs had left it with `Sound Init=0`, `EAX=0` and the movies marked as seen. Headless
  runs (and `--private-ini`) now read and write `work\swkotor.ini`, a fresh copy of the game's
  taken at the start of every run, so tests also start from the same settings.
- `--record` starts at the main menu. It used to fix its frame size on a 640x480 intro movie
  and crop everything after it.
- Injected keys reach the game in the area as well as in the menus. The game creates a second
  keyboard device when an area loads; keys now go to whichever keyboard device reads next.
- A machine with no audio device runs (silent by construction) instead of stopping because the
  mute could not be applied. Any other mute failure still stops the run.
- `wglSwapIntervalEXT` gets a no-op stand-in when the driver has none. The game calls it unchecked
  (0x0044E381), so under Windows' generic OpenGL 1.1 it called address 0. The host prints the GL
  vendor, renderer and version when the game's context becomes current.
- `--record` started on every frame check again after 24.8 days of Windows uptime: the frame
  pacing compared a signed `GetTickCount()` against 0.
- `kotor.exe` links the C runtime statically; a machine with no 32-bit VC++ redistributable
  could not start it (STATUS_DLL_NOT_FOUND).
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
