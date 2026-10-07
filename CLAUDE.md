# HaloWars2HDRPatch

Hook DLLs and a launcher (`HaloWars2HDRPatcher/`) that make Halo Wars 2's
mission briefing videos visible while Windows HDR stays on. The game is a UWP /
AppContainer DX12 title, `Microsoft.HoganThreshold_..._8wekyb3d8bbwe`, exe
`HaloWars2_WinAppDX12Final`. Package filter is `HoganThreshold`, not
`HaloWars2`. Payload logs land in `%LOCALAPPDATA%\Packages\<family>\AC\Temp\`.

The payloads apply the same fix and differ only in how they intercept DXGI.
The launcher's Strategy dropdown picks one, and its items are the DLL
suffixes:

| project | DLL | intercepts by |
| --- | --- | --- |
| `HaloWars2HDRPatchVtable/` | `HaloWars2HDRPatchVtable.dll` | patching DXGI's vtables |
| `HaloWars2HDRPatchCallsite/` | `HaloWars2HDRPatchCallsite.dll` | rewriting the host's own call sites; the launcher's default |
| `HaloWars2HDRPatchQI/` | `HaloWars2HDRPatchQI.dll` | handing the host proxy DXGI objects |

What they have in common — config, logging, the Bink gate, import patching,
`dllmain.cpp` and `Hooks.h` — lives once, in `HaloWars2HDRPatchShared/`, a
static library each DLL links. Each payload folder holds only its DXGI
interception and a CLAUDE.md for what is specific to it.

Read the code for how it works. This file is only what the code cannot tell
you: standing rules, and things measured on the testing machine.

---

## Rules

- **Allman braces**, everywhere, including single-statement `if`s.
- **VS 2026, x64 only.** `PlatformToolset` stays `$(DefaultPlatformToolset)` —
  do not pin a version number.
- **Build the `.sln`, never a `.vcxproj` directly.** A direct project build
  resolves `$(SolutionDir)` to the project folder, so the output lands
  elsewhere and a test then loads a stale binary from `build\`.
- **Build Debug *and* Release** after touching logging, the gate interface or
  `dllmain.cpp`. `HW2HDRPATCH_LOGGING` follows `_DEBUG`, so each configuration
  compiles code the other does not.
- **Nothing with side effects in a `Log` argument, and no function takes a
  parameter that exists only to be logged.** At `0` the whole call site
  vanishes, arguments included — `Log(L"%s", Describe(PatchSlot(...)))` would
  silently stop patching. Correctness rules, not style; building at `0` is how
  you find violations of the second one (C4100/C4189).
- **Release logs nothing; Debug logs every call, per frame included.** The
  Debug build exists to show which calls the host actually makes, so nothing in
  the diagnostics may summarise, deduplicate or suppress — a first-time-only
  line hides the repeats, and the repeat counts are the data. Measured: 42 fps
  through a gated movie either way. A session is tens of thousands of lines.
- **No timing.** No sleeps, no frame counts, no deadlines that anything is
  sequenced by, no polling heuristics. Hook the event. Two things are not this:
  a watchdog whose expiry only turns a hang into a diagnosable failure, and the
  launcher's short retry when Windows returns `ERROR_PARTIAL_COPY` for a
  just-created process's module list. That one works around how Windows reads
  a new process, not a guess about the game's timing.
- **The DLL owns no persistent threads.** Nothing may outlive install: a
  lasting thread would have to be stopped from `DLL_PROCESS_DETACH`, which runs
  under the loader lock, so it could not be joined.
- **Nothing mutates outside the gate.** Outside a gated movie every answer the
  host gets must be the one DXGI gave. A payload's only DXGI calls of its own
  are the ones that reach the objects it patches or wraps; if a change seems to
  need any other, the design is wrong, not the invariant.
- **Never call `SetColorSpace1` unsolicited.** Answer what the host asks;
  initiate nothing.
- **A checkbox is a claim that the answer varies.** Do not add an option for a
  configuration that is known-bad, or for an environment nobody has. Every
  injection option this project once had was removed for that reason.
- **No config file.** Config is one `|`-separated `key=value` string, and a
  default is a property of its own key, never of another key.
- **The launcher runs non-elevated.** `ActivateApplication` refuses to start
  Store apps from an elevated process, and the game is Low integrity under the
  same user anyway.
- Deploy to a short top-level path. Line endings are mixed and stay that way:
  payload sources are LF, every `.vcxproj` is CRLF, and the launcher is CRLF
  except `HooksConfig.cpp` and the `.rc`. Never flip a file wholesale.

## Requirements behind two design choices

- **The environment route exists for third-party injectors.** Any injector must
  be able to drive this DLL with nothing but `LoadLibrary` — no export name, no
  argument, no knowledge of internals. That is the whole reason `HW2HDRPATCH`
  and the `DllMain` self-install exist; the argument route stays the default.
  Our launcher's env-var route still keeps the game frozen through the install:
  it creates `HaloWars2HDRPatch.InstallComplete.<pid>` before the load, and the
  DLL sets it when `InstallOnce` finishes, success or not. Any other injector
  creates no such event, so its load races the host's startup.
- **`kBinkModule` is a constant, not a setting.** Halo Wars 2 ships exactly one
  Bink DLL and the name was *discovered* from the running process — nobody
  chose it. HW2 has no DLL plugins that touch Bink, which is also why watching
  for late-loading modules was never a useful goal.

---

## Measured on the testing machine

### The working configuration is the descriptor lie, without a downgrade

The fix is the `GetDesc1` lie plus the `CheckColorSpaceSupport` override that
SDR mode needs (below); nothing touches `SetColorSpace1`.

| configuration | result |
| --- | --- |
| descriptor lie | **correct** |
| swap-chain downgrade alone | desaturated — PQ Rec.2020 read as sRGB |
| pinning the back-buffer format | hangs or crashes at launch |
| blocking `SetHDRMetaData` | nothing; monitor metadata only |
| blocking `CheckOverlayColorSpaceSupport` | nothing; this host never calls it |

Left to itself, the host takes both its *encode* decision and its *label* from
the descriptor. When the gate opens it re-decides through its display-change
handler: `ResizeBuffers` to `R8G8B8A8_UNORM`, then `CheckColorSpaceSupport` and
its own `SetColorSpace1(SDR)`, all on the first poll after the gate opens. The
78 ms before that poll, in this Vtable session, is the render thread stalled in
`BinkOpen`, not the host deciding:

```
66.000  Present(sync 2, flags 0x0) (0x00000000)
66.015  BinkOpen("data\ui\skins\default\videos\MB_M00_Prologue.bk2", flags=0x01104000) gated
66.031  ==== gate OPEN ====
66.109  GetDesc1(0000040FF108A860) from HaloWars2_WinAppDX12Final.exe+0x81873, thread 26676 (0x00000000) [gate open]
66.109  CreateDXGIFactory1(IDXGIFactory) (0x00000000)
  ...
66.109  ResizeBuffers(2, 3440x1440, R8G8B8A8_UNORM, flags 0x800) (0x00000000)
66.109  CheckColorSpaceSupport(0 SDR) from HaloWars2_WinAppDX12Final.exe+0x81945 (0x00000000) [gate open]
66.109  SetColorSpace1(0 SDR) (0x00000000)
```

The `SetColorSpace1` downgrade was removed: it never fired, in HDR or SDR mode,
because while the gate is open the host never asks for HDR. In HDR mode it
re-decides from the lie and asks for SDR; in SDR mode the
`CheckColorSpaceSupport` override stops its one HDR request.
An older build measured the lie alone as black, but that one drove
`SetColorSpace1` from `Present` and so pre-empted the host.

`CheckColorSpaceSupport` matters in SDR mode: it zeroes HDR support once at
swap-chain creation, and the host then skips the `SetColorSpace1(HDR)` it
makes at that point in HDR mode.

**Not pixel-verified.** The videos looked correct; SDR versus HDR output was
not spot-checked.

### How the host answers the gate

- It polls `GetDesc1` **once per presented frame**, so it does not cache its
  encode decision at startup. That is the only reason a gate works at all; zero
  polls on some other host would mean the video-only fix is impossible there.
- **The slot sees twice that, and the second caller is DXGI.** An older build
  counted 1414 polls over 706 frames and could not say whose they were. The
  Debug log splits them between two callers:

  | | HDR session | SDR session |
  | --- | --- | --- |
  | host `GetContainingOutput` (`exe+0x81837`) | 8533 | 8257 |
  | host `GetDesc1` (`exe+0x81873`) | 8533 | 8257 |
  | DXGI's own `GetDesc1` (`dxgi.dll+0x437F8`) | 8542 | 8262 |
  | `Present` | 8557 | 8281 |

  DXGI calls `GetDesc1` on every output object it creates, on the same thread,
  before handing the object back. The host re-acquires its output every frame,
  so each `GetContainingOutput` carries one DXGI call on the output it then
  returns — 8533 of 8533 in HDR, 8257 of 8257 in SDR. The rest are one at
  swap-chain creation and two per display-change factory rebuild. A patched
  vtable slot sees them; a patched call site and a proxy object do not.
- It then **reallocates the back buffers**: `R8G8B8A8_UNORM` when the gate
  opens, `R10G10B10A2_UNORM` when it closes. The game keeps two whole render
  configurations and switches on the descriptor.
- It **re-asserts its own colour space** ~15 ms after a close, unprompted, so
  there is nothing for us to restore.
- Its own startup resize varies between runs (`3440x1360` → `3440x1440`, or
  not). That tracks whether the window ended up borderless. Do not read meaning
  into its absence.

### The host's whole path through DXGI

How the host reaches DXGI's objects, from a Debug session; among those routes,
anything not here it never took. The colour-space calls the fix answers are
covered above, and DXGI's own `GetDesc1` calls are not part of it.

- `CreateDXGIFactory1` (eight times a session) and `CreateDXGIFactory2` (once).
  Plain `CreateDXGIFactory` is **not in its import table** — an earlier build
  that patched all three found only two, which is why no payload lists it now.
  It asks for `IDXGIFactory`, `1`, `4`, `5`.
- One swap chain, always `CreateSwapChainForCoreWindow`: `3440x1360`,
  `R10G10B10A2_UNORM`, 2 buffers, flags `0x800`, flip-discard.
- **Per frame, exactly this: `GetContainingOutput` → QI `IDXGIOutput6` →
  `GetDesc1` → `GetCurrentBackBufferIndex` → `Present`.** The first three are
  1:1:1 with each other and with `Present`. It re-acquires the output every
  frame instead of caching it, and it never polls `GetDesc1` on an output that
  came from `EnumOutputs`.
- On every display change it **builds a whole new factory**:
  `CreateDXGIFactory1` → `EnumAdapters(0)` → `EnumOutputs(0)` → `GetDesc` →
  `ResizeBuffers`. A payload that follows the host's factories — QI wraps each
  one — therefore keeps its import patch live for the session.

### The AppContainer ACE is required

| | result |
| --- | --- |
| `F:\`, never granted, ACE absent | `LoadLibraryW` returns NULL, 3/3, never mapped |
| same binary after `icacls F:\Release /grant "*S-1-15-2-1:(OI)(CI)(RX)"` | **mapped, installed, hooks live** |

**A run that succeeds tells you nothing about whether a permission was needed;
only a run on a path that lacks it does.** `(OI)(CI)` propagates to
subdirectories created later and files copied in afterwards, so a "new folder"
inside an already-granted tree proves nothing. This was deleted once on the
strength of five such runs and had to come back. Keep the report line too: a
remote `LoadLibraryW` returning NULL carries no error code back, so without it
the failure is indistinguishable from a bad architecture or an AV quarantine.

### Activation and the environment

A broker-activated package is parented to `svchost.exe`, not to the requester —
the launcher never calls `CreateProcess`, so process-tree environment variables
are impossible. `HKCU\Environment` *does* reach a freshly activated package
immediately, with no `WM_SETTINGCHANGE` broadcast; changed values are picked up
and deletions propagate. Do not add the broadcast — it would tell every running
process to re-read an entry about to be deleted.

### The video pixels themselves cannot be fixed

Full white written into Bink's decoded planes displayed as flat RGB(16,16,16) —
0.52% of reference white, about **1 nit** against a 203-nit tonemap. Correct
output would need a plane value near 203.0 and the planes are 8-bit. No
transfer function, paper-white setting, gamut conversion or dither closes a
200x gap. That is why the fix is at the DXGI layer.

Only `MB_*` was ever black. Front-end and splash videos (`FE*`,
`Intro_Splash_*`) were always correct — same decoder, same geometry, different
compositing path — which is why `GateFilter` defaults to `MB_`.

### `BinkOpen`'s first argument is not always a path

One movie a session is opened from memory. Its `name` points at the video data,
not at a filename, and reading it as a string is a scan for a NUL through a
binary buffer:

```
[  124.859] BinkOpen("KB2jlñï", flags=0x04104000)
```

`KB2j` is the Bink 2 file magic. Every other call in that session had
`flags=0x01104000` and a real path, so the flag is what distinguishes them:
`kBinkFromMemory = 0x04000000`, **derived from this log line, not from a Bink
header** — the value is evidence, the name is a guess. `HookBinkOpen` tests it
and short-circuits, so `MovieMatchesFilter` never sees the pointer. Such a
movie is never gated, which is correct: there is no name to match, and the
gated ones all arrive as real paths.

Restore the check if `MovieMatchesFilter` is ever rewritten.

### Dirty pillarboxes are a game bug

*In its HDR path, Halo Wars 2 does not clear its fullscreen-video surface
between videos; the margins around a pillarboxed video hold the previous
video's final frame, normally black because cinematics fade out.*

Confirmed four ways: a control build that loaded but did not gate showed it;
skipping `Cine01` identified the stale pixels as `Cine01`'s own final frame;
letting `Cine01` play to the end makes the next video's bars correct; and it
reproduced unchanged on the stripped build. The pixels survived a fullscreen
`MB_M00_Prologue` and two whole swap-chain reallocations, so they are not in
the back buffer — they are in a game-owned surface this DLL has no handle to.
The letterbox is different: black in every configuration, so it is baked into
the video's own pixels.

**Do nothing about it.** `GateFilter=MB_;Cine;IGC.` would gate the cinematics
too, at the cost of HDR for those seconds. Untested and not needed.

Worth keeping because it cost four builds: every "clean" video ever observed
was either played before the gate first opened *or* stretched fullscreen with
no pillarbox to be dirty, so there was never a clean pillarboxed video to
compare against and the interceptor looked guilty by coincidence.

---

## Do not retry these

- **Third-party overlay injectors.** Their global paths wait on a shared-memory
  handshake an AppContainer's separate object namespace cannot reach, and their
  hook-based injectors are blocked by `DisableExtensionPoints`, which every
  AppContainer sets. Renaming one to `dxgi.dll` crashed the game.
- **A proxy / wrapper `dxgi.dll`.** It must sit next to the executable, which
  lives in `C:\Program Files\WindowsApps\...` — owned by `TrustedInstaller`,
  not even enumerable by the user. `dxgi.dll` is not in `KnownDLLs` (checked,
  37 entries), so that is not the blocker; the ACL is. It would also modify the
  installed package, which this project does not do.
- **Relabelling the swap chain alone** for the span of playback. Measured black.
- **A swap-chain downgrade in `SetColorSpace1`.** It never fires: while the
  gate is open the host never asks for HDR.
- **Converting Bink's planes**, in place or through shadow planes. In place it
  poisons Bink 2's prediction (frame N is motion-compensated from N-1); shadow
  planes fixed that and still ran into the 200x arithmetic above.
- **Touching `ResizeBuffers`.** Pinning the format crashes the game. Dropping an
  apparently redundant resize is also wrong — it is how a D3D12 app re-acquires
  back buffers it has released, so the call means more than its arguments.
- **Driving the gate from `Present`, or by frame count.** Measured: a close
  requested at 82.094 did not land until 82.359, with the next movie already
  open at 82.250 and the host's `ResizeBuffers` at 82.390.
- **`CREATE_SUSPENDED`.** A `CreateProcess` flag; the broker creates this
  process. It would also be *too early* — the loader has not run, so
  `CreateRemoteThread(LoadLibraryW)` would have nothing to call.
- **`IPackageDebugSettings::EnableDebugging`'s `debuggerCommandLine`**, the
  packaged-app equivalent. **Ruled out by the user.** Noted only so it is not
  proposed again.
- **Reintroducing the module watcher.** It covered one scenario — a plugin DLL,
  loaded after install, statically importing Bink — which this game does not
  have, and cost a thread for the life of the process plus a callback under the
  loader lock on every module load.
- **Any diagnostic that reports a call only the first time.** It was tried and
  removed: the repeat *is* the finding — DXGI's `GetDesc1` pairing one-to-one
  with `GetContainingOutput` above could not be read off first-time-only lines.

---

## Traps that are not visible in the source

- `rc.exe` silently truncates dialog control text at 255 characters, mid-word,
  with no diagnostic. Nothing is near that now; keep it that way.
- **A dialog unit is not the same number of pixels in the two templates** —
  `IDD_MAIN` is 9pt Segoe UI, `IDD_TESTING` is 8pt MS Shell Dlg. `LayoutView`
  measures the panel with `GetWindowRect` for that reason. Only `IDD_MAIN`'s
  own size may be a constant; the panel's must not be re-added.
- `IDC_STATIC` is `-1`, so `GetDlgItem(dialog, -1)` cannot pick one static out
  of several. Anything `LayoutView` shows or hides needs a real control id.
- `StatusIcon.h` needs `NOMINMAX` before `windows.h`.
- **The install event is the launcher's to create, in the game's namespace.**
  Inside an AppContainer an unqualified object name resolves to the
  container's own directory, so the launcher makes it at the path
  `GetAppContainerNamedObjectPath` gives. The sandbox can only set it with an
  ACE for Everyone *and* one for ALL APPLICATION PACKAGES, plus a low
  integrity label. The DLL does not create it because the launcher would then
  have to poll for it.
- **`dllmain.cpp` is compiled into each DLL, not into the shared library.** From
  a `.lib`, nothing pulls it in before the CRT's own default `DllMain` stub
  satisfies the reference, so the linker drops it and the DLL builds cleanly
  with no exports — neither install route can then run. Check with
  `dumpbin /exports` for `HaloWars2HDRPatchInstall` after touching the projects.
- Prefer a namespace-scope `SRWLOCK g_x = SRWLOCK_INIT;` to a lock behind a
  function-local `static`. `SRWLOCK_INIT` is `{0}`, so it is constant-
  initialised before any code in the image runs; a function-local static is
  initialised lazily under its own guard, which is one more thing to race.
- The simple view is deliberately one column; anything wider belongs in the
  advanced panel.
