# KotOR (PC) reconnaissance

Reconnaissance of the Steam copy (`steamapps\common\swkotor`),
done 2026-10-03 with pcrecomp `origin/main` (1fc3a0a). Nothing has been lifted yet. The Steam DRM is removed (see below).

## The binary

| | |
|---|---|
| File | `swkotor.exe`, 4,395,008 bytes, SHA1 `440fe2b3a0c60849cfb996c4cf580fdcb9fd4ebb` |
| Build | PE32 i386 GUI, linker 7.00 (VC.NET 2002), timestamp 2004-02-12 18:15:53 UTC (1.03) |
| Code | `.text` 0x401000-0x73D000, 3.39 MB, **encrypted** (entropy 7.998) |
| Data | `.rdata` 320 KB, `.data` 673 KB virtual (92 KB raw), `.rsrc` 219 KB |
| Wrapper | `.bind` 0x86D000, 344 KB, holds the entry point: SteamStub 2.x |
| Imports | 350 from 11 DLLs: OPENGL32 (95), KERNEL32 (113), USER32 (40), mss32 (61), binkw32 (20), GDI32, GLU32, DINPUT8, IMM32, VERSION, ole32 |
| RTTI | Off. `cpp/rtti.py` finds only 5 CRT classes |
| Names in strings | Few: ~75 `CExo*`/`CSWS*` strings in asserts and logs, 3 `.cpp` names |

The renderer is fixed-function OpenGL 1.x plus vendor extensions (NV register combiners,
`GL_ATI_fragment_shader`). Audio is Miles 6.5 and video is Bink. Input is DirectInput 8.
None of these is a Direct3D-era COM API, so the native32 host can forward every one of them to the real DLL.

Other binaries: `swconfig.exe` (MFC config tool, linker 5.0), `swupdate.exe`, and
`utils/swstub.exe`. None of them is needed to run the game.

## The Steam wrapper (SteamStub 2.x, x86)

The detector in `pe/analyze_sections.py` used to label `.bind` "SafeDisc wrapper". It now says
SteamStub (fixed in #46).

What the stub does, read from its code and confirmed under emulation (an emu_unpack harness, kept locally in the gitignored `scratch/`):

1. **Header.** 0x364 bytes at VA 0x86F204, encoded as a running XOR with seed 0x067262EF
   (`v ^= key; key = old v`). Fields seen so far:
   - +0x04..0x14: pointers into the original IAT, used for LoadLibraryA and GetProcAddress
   - +0x18: `.bind` VA; +0x1C: stub size 0x2204
   - +0x20: stub checksum 0x03C1C421
   - +0x24: payload VA 0x8C1168; +0x28: payload size 0x1000
   - +0x34: app id string `"32370"`
   - then DLL and API name strings, `Local\SteamStart_SharedMem*`, and `-applaunch`
2. **Checks.**
   - It checksums the stub and compares the result with +0x20.
   - Anti-debug: reads `TEB->PEB->BeingDebugged`. A null PEB also counts as debugged.
   - Looks for a `-drm?` switch on the command line.
3. **Steam handshake.** If the payload's first dword is not the checksum, the stub opens the
   SteamStart shared memory and waits for Steam to write it. On disk the dword already
   matches, so this step is skipped.
4. **Payload.** The 4 KB block is running-XOR decoded with the checksum as the key. The stub
   then XTEA-decrypts a 0x51C00-byte PE and loads it in memory at 0x10000000. That PE is
   `steamdrm.dll` (PDB `d:\s3_main\bin\util\drm\steamdrm.pdb`), built on Crypto++:
   Rijndael/AES-CBC, plus an RSA PKCS#1 SHA-1 verifier.
5. **steamdrm.** The stub calls a steamdrm export with (payload, 0x1000), and the export
   returns the OEP. Inside it:
   - runs its own anti-debug: `NtSetInformationThread(ThreadHideFromDebugger)`
   - reads its own exe from disk
   - runs `SteamAPI_Init`, which looks up `HKCU\Software\Valve\Steam\ActiveProcess` and
     `SteamClientDll`, and wants `SteamClient008` / `SteamUtils004`
   - on failure, `ShellExecuteA("steam.exe", "-applaunch 32370")` and exits.

6. **The key is in the file.** steamdrm reads everything from the payload, at offsets compiled into its code:
   - +0x574: AES-256 key; +0xBAC: first encrypted block
   - +0x6EC: OEP 0x6FB38D; +0xC98: code VA 0x401000; +0xB34: code size 0x33BFF0
   - +0xE54: app id; +0xDF8: flags (bit 4 means the code isn't encrypted)

   The code section is AES-256-CBC, with the IV ECB-decrypted from the first block.

**Removed** by pcrecomp `tools/drm/steamstub.py` (PR sp00nznet/pcrecomp#46, branch
`feat/steamstub`). It is static, needs no Steam, and takes under a second:

```
SteamStub 2.x: app 32370, header 0x0086F204, steamdrm.dll 0x0086F568 (0x51C00 bytes)
  .text 0x00401000+0x33BFF0 decrypted (AES-256-CBC); OEP 0x006FB38D
```

The unwrapped exe boots without Steam: windowed on an offstage monitor, it stayed up
over 60 s and loaded assets. `disasm32` on it finds 29,594 functions and covers 95.9% of
the code range (2.61M instructions; 13,608 C++ EH funclets).

**offstage caveat:** KotOR changes the display mode at startup (probably the 640x480
switch for the intro movies). The virtual monitor dropped out and recording stopped
after 1 s. So until that's solved, boot tests need `FullScreen=0` and a check that
the window stays on the virtual monitor.

## Game data (not lifted; read by the engine at runtime)

3.4 GB. `chitin.key` + 26 BIFs in `data/` (1.3 GB), 234 module files (`.rim`/`.erf`/`.mod`),
`dialog.tlk`, `patch.erf`, 61 Bink movies, and 13,799 VO waves. Game logic is NWScript
bytecode (`.ncs`) running on the engine's VM, so content bugs are data fixes and need
no recompilation. K1CP already fixes them.

## Community material (checked for licence)

| Source | What | Licence / use |
|---|---|---|
| Lane's GOG Ghidra project (deadlystream topic 11948) | 99.9% of functions labelled, partial structs, `.h` | None stated; reference only. GOG exe; Steam addresses reported "mostly identical" once the DRM is off |
| LaneDibello/Kotor-Patch-Manager | SQLite address DB for 1.03 PC | **MIT**, usable |
| Synchro's K1 Modern Driver Compatibility patch | Renderer RE: grass, `ARB_fragment_program` paths | MPL-2.0; files may stay MPL, keep them separate |
| reone, xoreos | Engine reimplementations | GPL-3.0; don't copy |
| KotOR-dotNET, Vanilla_KOTOR_Script_Source, KotOR Scripting Tool | Format parsers, decompiled `.nss`, nwnnsscomp GUI | No licence; reference only |

## Known engine-level problems (fix candidates)

1. **No frame cap above 60 fps.** Movement freezes after combat, and physics behaves oddly.
   The limiter float at VA 0x7A3C64 stays at 0.0. The code that sets it is guarded by a flag
   nothing ever writes (VexFlint).
2. **Grass streaks across the sky.** Two of the three `RenderGrassPolys` paths pass the
   wrong field (+0x40, always 0) to `glVertexPointer` instead of +0x38 (Synchro).
3. **Lighting, fog, reflections, soft shadows and Frame Buffer Effects missing or
   crashing.** Only the NV and ATI shader paths are finished, and today's drivers expose
   neither (Synchro rebuilds them on ARB_fp).
4. **4:3 resolutions only, and the UI doesn't scale.** Community fixes: UniWS and High
   Resolution Menus.
5. **Movies** need 640x480 available, can come up black in widescreen, and stretch.
6. **Multi-core lockups.** Workaround: pin the game to one CPU (affinity).
7. **Hardware mouse problems**, plus lag after cutscenes. Workaround: VBOs off.
8. **No controller support.**
9. **Steam:** the DRM blocks every exe patch above.

Source list: PCGamingWiki's KotOR page and the deadlystream threads above.
