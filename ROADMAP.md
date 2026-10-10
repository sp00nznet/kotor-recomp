# Roadmap

## Bring-up

In order; each is done when its check is in the conformance harness or the docs.

1. **Open the footlocker.** Trask's footlocker tutorial now answers in both the lifted run
   and `--original` (docs/testing.md); script selecting and opening the footlocker, and make
   it a harness milestone.
2. **Walk on the Endar Spire** with W, then reach the first fight, all as harness milestones.
3. **The gamepad on a real controller**, then menu navigation by D-pad.
4. **Sound tests.** Every run is muted until then (`--sound` unmutes).
5. **Move oracle.c into pcrecomp's native32**: civ3, Red Alert 2 and KotOR each carry
   a copy.

## Fixes (the reason for the project)

Each is a patch in `run_lift.py` or a host hook, off by default until it has a
check. The list and the evidence for each is in [docs/RECON.md](docs/RECON.md).

- Frame cap at the refresh rate (the limiter at 0x007A3C64 is never armed).
- Grass drawn from the wrong vertex pointer on two of three paths.
- Lighting, fog, soft shadows and framebuffer effects on the
  `ARB_fragment_program` path, which today's drivers expose.
- Widescreen and high resolution, with the UI scaled.
- Movies: no 640x480 mode switch, no stretching.
- Multi-core lockups (the reason players set the affinity to one core).
- Controller support.
- Windowed mode: the cinematic backdrop paints the desktop black. Its window class is never
  registered (`0x00401B37`), so it draws on `GetDC(NULL)` instead; register the class or skip
  the backdrop when windowed.

## Deferred

- The Mac (Aspyr) and Xbox builds.
- KotOR II: a separate repo, the way each Westwood game is its own.

## Out of scope

- Content and script bugs: they live in the game data, and the KotOR 1
  Community Patch already fixes them.
- Distributing anything from the game: no exe, no lifted C, no assets.
