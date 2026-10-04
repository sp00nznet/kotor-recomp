# Architecture

Three parts, two languages, one process at run time.

```
Steam install ──(junction)── game\           read in place: chitin.key, BIFs, modules, swkotor.ini
       │
       └─ swkotor.exe ──steamstub.py──► work\swkotor.exe ──vtable_scan + disasm32──► work\functions.json
                                                                                         │
                                                                     run_lift.py (lift32)│
                                                                                         ▼
                                                                         src\recomp\gen\*.c (gitignored)
                                                                                         │
                                       src\runtime\host.c + pcrecomp runtime/native32 ───┤ build.cmd (MSVC x86)
                                                                                         ▼
                                                                                 build\kotor.exe
```

## The pipeline (Python, pcrecomp)

- **`tools/drm/steamstub.py`** removes SteamStub 2.x without running anything.
  KotOR's Steam exe AES-encrypts its whole `.text`; the key is in the file
  (docs/RECON.md has the layers).
- **`tools/cpp/vtable_scan.py`** finds the vtables. KotOR has no RTTI, and a
  method reached only through a vtable is named by no `call`, so its slots seed
  the catalog.
- **`tools/disasm/disasm32.py`** catalogs the functions. No IDA or Ghidra is
  involved; Lane's Ghidra project for the GOG exe is a reference to score
  against, not an input (it has no licence).
- **`run_lift.py`** lifts the catalog with `lift32` into chunked C files plus a
  dispatch table. It is the same driver as Red Alert 2's and The Movies'.

## The host (C, 32-bit)

`build\kotor.exe` is a 32-bit process linked at 0x60000000, so the guest image
maps 1:1 at its own base, 0x00400000. pcrecomp's `native32` binds the guest's
IAT to the real DLLs: OpenGL, Miles (`mss32.dll` from the game folder), Bink,
DirectInput. A native call copies the guest's argument slots to the host stack
and measures the purge, so no per-import table is needed. Windows calling back
into the game (the window procedure, threads) faults on the non-executable
guest code and is redirected into the lifted body.

`src/runtime/host.c` holds only what is specific to KotOR:

- **The module is the guest, not the host**: `GetModuleHandleA(NULL)`,
  `GetModuleFileNameA` and `GetCommandLineA` answer for `game\swkotor.exe`.
- **Nothing outside the window.** `ChangeDisplaySettingsA` changes the primary
  display, not the one the window is on. It knocked an offstage virtual monitor
  out mid-recording and would resize a real screen. `SetDeviceGammaRamp`
  changes a real monitor's gamma and survives a crash. Both are answered
  "done" without doing anything unless `--modeswitch` / `--gamma` is given.
- **Muted by default.** The process's own audio session is muted before the
  game starts, which covers Miles, Bink and anything else. If muting fails the
  run stops (fail closed). `--sound` turns it off.
- **Headless.** `--headless` keeps the window hidden and prints message boxes.
  OpenGL still renders into the hidden window.
- **Reports.** Fault reports, a watchdog, and a not-lifted stub that names the
  next function to lift.
