# Roadmap

## Bring-up

In order; each is done when its check is in the conformance harness or the docs.

1. **Boot to the first frame.** The lift runs from `WinMainCRTStartup` to a GL
   context and a presented frame, headless and muted (`tools/conformance.py`).
2. **Boot to the main menu**, then a new game started and the Endar Spire loaded.
3. **Headless recording**: `--headless --record out.mp4`, frames read back
   from the hidden GL window and piped to ffmpeg (REPO_RULES section 10).
4. **A reference run** of the unwrapped original under offstage, to compare
   frames and logs against.
5. **Sound tests.** Every run is muted until then (`--sound` unmutes).

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
