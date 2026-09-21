# HaloWars2HDRPatch

A hook DLL (`HaloWars2HDRPatch/`) and a launcher (`HaloWars2HDRPatcher/`) that
make Halo Wars 2's mission briefing videos visible while Windows HDR stays on.
The game is a UWP / AppContainer DX12 title, `Microsoft.HoganThreshold_...
_8wekyb3d8bbwe`, exe `HaloWars2_WinAppDX12Final`. Package filter is
`HoganThreshold`, not `HaloWars2`. Payload logs land in
`%LOCALAPPDATA%\Packages\<family>\AC\Temp\`.

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
  compiles code the other does not, and one of them once did not compile at all.
- **Nothing with side effects in a `Log` argument, and no function takes a
  parameter that exists only to be logged.** At `0` the whole call site
  vanishes, arguments included — `Log(L"%s", Wrap<OutputProxy, ...>(output)
  ? ... : ...)` would silently stop wrapping. That is why every `Wrap*` helper
  lands the result in a `[[maybe_unused]] const bool` first. Correctness rules,
  not style; building at `0` is how you find violations of the second one
  (C4100/C4189).
- **Release logs nothing; Debug logs every call, per frame included.** The
  Debug build exists to show which calls the host actually makes, so nothing in
  the diagnostics may summarise, deduplicate or suppress — a first-time-only
  line hides the repeat that was the finding. Measured cost so far: 42 fps
  through the gated movie with per-frame logging and 42 fps without, so at this
  frame rate the per-line write does not show. A session is tens of thousands
  of lines.
- **No timing.** No sleeps, no frame counts, no deadlines that anything is
  sequenced by, no polling heuristics. Hook the event. (A watchdog whose expiry
  only turns a hang into a diagnosable failure is fine and is not this.)
- **The DLL owns no persistent threads.** Nothing may outlive install: a
  lasting thread would have to be stopped from `DLL_PROCESS_DETACH`, which runs
  under the loader lock, so it could not be joined.
- **Nothing mutates outside the gate.** Outside a gated movie the DLL must be
  indistinguishable from not being injected. If a change seems to need a DXGI
  call outside the gate, the design is wrong, not the invariant.
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
- Deploy to a short top-level path. Line endings: the launcher project is CRLF,
  the DLL project is LF — do not flip either wholesale.

## Requirements behind two design choices

- **The environment route exists for third-party injectors.** Any injector must
  be able to drive this DLL with nothing but `LoadLibrary` — no export name, no
  argument, no knowledge of internals. That is the whole reason `HW2HDRPATCH`
  and the `DllMain` self-install exist; the argument route stays the default.
- **`kBinkModule` is a constant, not a setting.** Halo Wars 2 ships exactly one
  Bink DLL and the name was *discovered* from the running process — nobody
  chose it. HW2 has no DLL plugins that touch Bink, which is also why watching
  for late-loading modules was never a useful goal.

---

## Measured on the testing machine

### The working configuration is a pair

| configuration | result |
| --- | --- |
| descriptor lie **and** swap-chain downgrade | **correct** |
| descriptor lie alone | black — **superseded; see the next section** |
| swap-chain downgrade alone | desaturated — PQ Rec.2020 read as sRGB |
| pinning the back-buffer format | hangs or crashes at launch |
| blocking `SetHDRMetaData` | nothing; monitor metadata only |
| blocking `CheckOverlayColorSpaceSupport` | nothing; this host never calls it |

The host decides how to *encode* from `GetDesc1` and how to *label* the chain
from `SetColorSpace1`, fed by different code. Lie to one and not the other and
it is internally inconsistent.

### Why "descriptor lie alone" measured black

That row was measured under a build that pre-empted the host. The pair in the
first row is still a correct configuration — it is no longer the minimal one.

The vtable build drove the colour space itself, from `Present`
(`>>> gate OPEN: chain now sRGB`, 63 ms after the request), on a premise
written in its own source: *"The host does not call SetColorSpace1 again after
startup, so the transition has to be driven from here."*

**That premise is false for the wrapper build.** The host re-decides on its
own, through its display-change handler, 62 ms after the gate opens:

```
48.954  ==== gate OPEN ====
49.016  host ResizeBuffers(2 buffers, 3440x1440, R8G8B8A8_UNORM, ...) [gate open]
49.016  host SetColorSpace1(0 SDR) [gate open]
```

So the downgrade branch never executes: no `downgraded to sRGB` line appears in
an HDR-mode or an SDR-mode session. The descriptor lie alone put the host in
the configuration the first row calls correct. The `CheckColorSpaceSupport`
override is not idle though — in SDR mode it zeroes HDR support once at
swap-chain creation, and the host then skips the `SetColorSpace1(HDR)` it makes
at that point in HDR mode.

**Not pixel-verified.** The videos looked correct; SDR versus HDR output was
not spot-checked. What is established is that the host reaches the right
configuration, not that the pixels leaving it were measured.

### How the host answers the gate

- It polls `GetDesc1` **once per presented frame** — 358 polls over 358
  `Present` calls through a gated movie, 4602 over 4624 across a session — so
  it does not cache its encode decision at startup. That is the only reason a
  gate works at all; zero polls on some other host would mean the video-only
  fix is impossible there.
- **The vtable build measured twice that rate**: 1414 polls over 706 frames,
  same movie, same 42 fps. Neither counter double-counted (one
  `InterlockedIncrement64` each, checked in the old source), and a vtable patch
  writes the *shared* vtable, so its single install line is what you would see
  with one caller or ten — the install log cannot settle it. What fits is a
  second per-frame caller polling an `IDXGIOutput6` that never came through our
  factory, which a global patch catches and a wrapper cannot. The decision path
  still flips at the gate, so it does not appear to matter; nothing proves it.
- It then **reallocates the back buffers**: `R8G8B8A8_UNORM` when the gate
  opens, `R10G10B10A2_UNORM` when it closes. The game keeps two whole render
  configurations and switches on the descriptor.
- It **re-asserts its own colour space** ~15 ms after a close, unprompted, so
  there is nothing for us to restore.
- Its own startup resize varies between runs (`3440x1360` → `3440x1440`, or
  not). That tracks whether the window ended up borderless. Do not read meaning
  into its absence.

### The host's whole path through DXGI

Every route it takes, from a Debug session. Anything not here, it never did.

- `CreateDXGIFactory1` (eight times a session) and `CreateDXGIFactory2` (once).
  Plain `CreateDXGIFactory` is **not in its import table** — the patch reports
  two slots, not three. It asks for `IDXGIFactory`, `1`, `4`, `5`; never 2, 3,
  6 or 7.
- Adapters through `EnumAdapters` / `EnumAdapters1`, QI'd to `IDXGIAdapter`,
  `1`, `3`. D3D12 then asks each adapter for `IDXGICoreAdapter`
  (`{F0DB4C7F-FE5A-42A2-BD62-F2A6CF6FC83E}`) and
  `{23FDBE30-0292-491B-89F2-AEECFCFA11C6}` — not RenderDoc, which is not
  installed on the testing machine.
- One swap chain, always `CreateSwapChainForCoreWindow`: `3440x1360`,
  `R10G10B10A2_UNORM`, 2 buffers, flags `0x800`, flip-discard. QI'd to
  `IDXGISwapChain1`, `3`, `4`; never 2.
- **Per frame, exactly this: `GetContainingOutput` → QI `IDXGIOutput6` →
  `GetDesc1` → `GetCurrentBackBufferIndex` → `Present`.** The first three are
  1:1:1 with each other and with `Present`. It re-acquires the output every
  frame instead of caching it, and it never polls `GetDesc1` on an output that
  came from `EnumOutputs`.
- On every display change it **builds a whole new factory**:
  `CreateDXGIFactory1` → `EnumAdapters(0)` → `EnumOutputs(0)` → `GetDesc` →
  `ResizeBuffers`. The import patch therefore has to stay live for the session,
  not just for startup.

`GetDesc1` exists only on `IDXGIOutput6`, and the host asks for `IDXGIOutput6`
by name 4602 times a session. There is no older interface to fall back to, and
the level a wrapper can wrap at is a property of the OS's DXGI, not of the
game's 2017 vintage.

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

Nothing in the source hints at this. Restore the check if `MovieMatchesFilter`
is ever rewritten.

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
- **Wrapping the adapter.** `AdapterProxy` existed only so `EnumOutputs` would
  hand back wrapped outputs, and the host never polls `GetDesc1` on those — the
  1:1 count between `GetContainingOutput` and `GetDesc1` is the proof. It was
  also the only object anything ever asked for an interface we do not answer.
- **Unwrapping the adapter for `D3D12CreateDevice`.** Proposed on the theory
  that D3D12 cannot use a proxy adapter. No citation was ever found for it, and
  the logs settle it the other way: with the adapter wrapped, D3D12 asked for
  its private interfaces, got the real object through the fall-through, created
  the device and rendered 4600 frames.
- **Any diagnostic that reports a call only the first time.** It was tried and
  removed: the repeat *is* the finding — one poll per frame versus two is the
  whole question above, and a first-time-only line cannot show it.

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
- Prefer a namespace-scope `SRWLOCK g_x = SRWLOCK_INIT;` to a lock behind a
  function-local `static`. `SRWLOCK_INIT` is `{0}`, so it is constant-
  initialised before any code in the image runs; a function-local static is
  initialised lazily under its own guard, which is one more thing to race.
- The simple view is deliberately one column; anything wider belongs in the
  advanced panel.
