# HaloWars2HDRPatch

Halo Wars 2's mission briefing videos render almost black when Windows HDR is
on. This fixes them, with HDR left on.

## Install

Copy these files into a short folder you own, such as `C:\HaloWars2HDRPatch\`:

```
HaloWars2HDRPatcher.exe
HaloWars2HDRPatchVtable.dll
HaloWars2HDRPatchCallsite.dll
HaloWars2HDRPatchQI.dll
```

Only the DLL your chosen strategy names is needed; the default is Callsite.

Avoid OneDrive and deep user-profile paths. If the files came out of a ZIP,
right-click each one, Properties, **Unblock**.

## Run

Start `HaloWars2HDRPatcher.exe` **as a normal user**, not as administrator —
Windows will not launch Store apps from an elevated process.

Pick a mode and press **Launch Halo Wars 2**:

- **HDR** (default) — fixes the briefing videos; the rest of the game keeps HDR.
- **SDR** — the whole session runs in SDR. Try it if HDR mode does not work.

**Strategy** picks how the patch hooks the game — Vtable, Callsite or QI. All
three apply the same fix; leave it on Callsite unless that one does not work.

The indicator turns green once the patch is in, and the game takes the
foreground. You can close the launcher; the game keeps running.

Everything under **Advanced** is for troubleshooting.

## Notes

- Skipping a cinematic can leave stale frames in the bars beside the next
  video. That is a game bug and happens without this tool.
- Most of this code was written by Claude.