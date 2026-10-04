# Gamepad

KotOR on PC has no controller support. The host adds it: an XInput pad (Xbox controllers and
anything that presents as one) is read 120 times a second and turned into the game's own keys,
its cursor and its mouse button. The game itself is unchanged; it sees a keyboard and a mouse.

The pad is on whenever the window is shown. `--headless` runs leave it off, so a test never reads
a pad that happens to be plugged in. `--gamepad` turns it on for a headless run and
`--no-gamepad` turns it off for a shown one. The host prints `[gamepad] connected` when it finds
the pad and `not connected` when it loses it. Only the first pad is read.

## The mapping

Keys are the game's defaults from `keymap.2da` (`py -3 tools\keymap.py` prints the table), so
rebinding a key in the game's options moves it off the pad too.

| Pad | Game action | Key sent |
|---|---|---|
| Left stick up / down | move forward / back (ActionUp / ActionDown) | W / S |
| Left stick left / right | strafe (ActionLeft / ActionRight) | Z / C |
| LT / RT | rotate the camera (CameraRotateLeft / Right) | A / D |
| Right stick | the mouse cursor, faster the further it's pushed | (cursor) |
| A | left click: menus, dialogue, picking a target, moving to a spot | (mouse button) |
| B | back / close (GUI) | Escape |
| X | default action on the target: talk, open, attack (DefaultAction) | R |
| Y | pause (Pause) | Space |
| LB / RB | previous / next target (SelectPrev / SelectNext) | Q / E |
| D-pad left / up / right | the target's three action slots; dialogue replies 1-3 | 1 / 2 / 3 |
| D-pad down | personal power; dialogue reply 4 | 4 |
| Back | change party leader (ChangeChar) | Tab |
| Start | the game menu | Escape |

## Tuning

The knobs are at the top of the gamepad section of `src/runtime/input.c`:

| Constant | Default | What it is |
|---|---|---|
| `PAD_STICK_DEADZONE` | 9000 of 32767 | a resting stick reads up to about 7800 on a worn pad; raise it if a character drifts |
| `PAD_TRIGGER_ON` | 60 of 255 | how far a trigger travels before the camera turns |
| `PAD_CURSOR_SPEED` | 12 | pixels per poll at full deflection; the curve is squared, so small pushes stay precise |

The mapping table is `g_pad_buttons` (buttons) and `pad_step` (sticks and triggers) in the same
file. `kotor.exe --selftest-input` checks it.

## How it reaches the game

- **Keys:** the game reads the keyboard with DirectInput, buffered (`GetDeviceData` every frame).
  The host hooks the keyboard device's `GetDeviceData` and appends the pad's key events after
  the real ones, so the keyboard keeps working alongside the pad.
- **Cursor and clicks:** the game takes the mouse from window messages, so the pad posts
  `WM_MOUSEMOVE` and the button messages to the game's window. When the window is shown and
  focused, it moves the real cursor too.

## Not done yet

- Walking with the left stick is digital (the game has no analog movement), so a light push
  walks at full speed.
- The menus have no pad navigation of their own; the cursor and A do it.
- Rumble, and more than one pad.
