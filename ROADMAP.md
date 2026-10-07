# Roadmap

## Bring-up

In order; each is done when its check is in the conformance harness or the docs.

1. **The footlocker conversation.** The second tutorial conversation shows its reply but
   takes no input of any kind (docs/testing.md). The suspect is a script command that never
   resumes a paused conversation. That needs the oracle (next item) to compare against.
2. **The oracle takes scripted input.** Under `--original` the shipping code still ignores
   the posted clicks, even with the focus and activation messages. Until it takes them, a
   scripted run can't be compared with it, and that comparison is what the step above needs.
3. **Walk on the Endar Spire** with W, then reach the first fight, all as harness milestones.
4. **The gamepad on a real controller**, then menu navigation by D-pad.
5. **The visible window.** Headless runs play, but with a shown window the lifted run
   stalls in window creation (0x004053E0, the window procedure called over and over)
   while the original does not. Find what differs.
6. **Sound tests.** Every run is muted until then (`--sound` unmutes).
7. **Move oracle.c into pcrecomp's native32**: civ3, Red Alert 2 and KotOR each carry
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

## Deferred

- The Mac (Aspyr) and Xbox builds.
- KotOR II: a separate repo, the way each Westwood game is its own.

## Out of scope

- Content and script bugs: they live in the game data, and the KotOR 1
  Community Patch already fixes them.
- Distributing anything from the game: no exe, no lifted C, no assets.
