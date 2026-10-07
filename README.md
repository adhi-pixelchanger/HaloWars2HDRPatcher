# HaloWars2HDRPatch

Halo Wars 2's mission briefing videos render almost black when Windows HDR is
on. This fixes them, with HDR left on in Windows, but giving you the option
to still play HW2 in either SDR or HDR.

## Run

Extract all and start `HaloWars2HDRPatcher.exe`.

Pick a mode and press **Launch Halo Wars 2**:

- **HDR** (default) — fixes the briefing videos; the rest of the game keeps HDR.
- **SDR** — the whole session runs in SDR.

**Strategy** auto-picks how the patch hooks the game — Vtable, Callsite or QueryInterface,
based on detected Windows permissions.  If one strategy fails to work on your PC, 
manually set a different one.

The indicator turns green once the patch is in, and the game takes the
foreground. You can close the launcher; the game keeps running.

Everything under **Advanced** is for troubleshooting, logging requires a debug build,
this project was built with VS 2026.

## Notes

- Skipping a cinematic can leave stale frames in the bars beside the next
  video. That is a game bug with HDR and happens without this tool.
