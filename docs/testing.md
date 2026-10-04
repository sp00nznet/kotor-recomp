# Testing

`tools/conformance.py` runs `kotor.exe --headless --run` (muted, hidden window) with a fixed
click script, and scores the run by the lines the host prints. It fails if any of these get
worse than `conformance.json`: the milestone count, lift errors, functions with no
terminator, or unresolvable tail-dispatch labels.

```
py -3 tools\conformance.py                          # build\kotor.exe, 240 s
py -3 tools\conformance.py --host D:\kotor.exe --seconds 300 --update
```

```
boot milestones: 11/11  (exit 4)
  [x] image mapped and imports bound
  [x] audio muted
  [x] entry point entered
  [x] window created
  [x] intro movies opened (LucasArts, BioWare, legal)
  [x] first frame presented
  [x] main menu music started
  [x] 1000 frames presented
  [x] Endar Spire module loaded
  [x] opening conversation started
  [x] in game: frames presented after the module loaded
  stopped: [watchdog] 300 s: in sub_0070AB30, last native call KERNEL32.dll!Sleep, 148330325 indirect calls, 30745 frames
lift: 29617 functions, 0 errors, 159 with no terminator, 1 unresolvable ITAIL labels
```

## Where the milestones come from

Each one is a line the host prints when the game does something it does on every start.

| Line | Printed by | What the game is doing |
|---|---|---|
| `[host] movie .\movies\legal.bik -> open` | `BinkOpen` shim | the third intro movie |
| `[host] stream \\N,1405913.mp3 -> open` | `AIL_open_stream` shim | the main menu's theme, a Miles stream out of an archive |
| `[host] module .\Modules\END_M01AA.rim -> open` | `CreateFileA` shim | the Endar Spire loading after Play |
| `[host] module .\lips\END_M01AA_loc.mod -> open` | `CreateFileA` shim | lip sync for the opening conversation (Trask) |
| `[host] frame N presented after a module load` | `SwapBuffers` shim | the first frame drawn after a module opened |

## The new-game script

Clicks are in client coordinates of the 800x600 window, top-left origin, the same as a
`--record` frame. Times are in seconds after the main menu appears: the host starts the clock
when the menu music opens, because the intro takes anywhere from 30 s to 90 s depending on
how busy the machine is. Each click is a move, then a press and a release 100 ms apart,
posted to the window the game renders into.

| `--click` | Screen | Button |
|---|---|---|
| `488,293@3` | main menu | New Game |
| `180,280@12` | Choose your class | Male Scoundrel (the first portrait) |
| `180,280@18` | Choose your class | the same portrait again, to confirm |
| `536,186@25` | Quick / Custom | Quick Character |
| `555,222@32` | Quick character steps | 1 Portrait |
| `264,485@39` | Portrait | OK |
| `555,252@46` | Quick character steps | 2 Name (the screen comes up with a random name) |
| `272,485@53` | Name | OK |
| `555,283@60` | Quick character steps | 3 Play |

To find a button, record a run and pick the coordinates off a frame:

```
build\kotor.exe --run --headless --watchdog 120 --record out.mp4 --click 488,293@3
ffmpeg -i out.mp4 -vf "fps=1/3,scale=200:-1,tile=8x4" -frames:v 1 sheet.png
```

## Known gaps

- Under `--original`, the shipping code does not react to the posted clicks, so scripted runs
  can't be compared with it yet (ROADMAP).
- Keyboard input goes through DirectInput, not window messages; the script has no keys yet.
