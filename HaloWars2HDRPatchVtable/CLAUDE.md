# HaloWars2HDRPatchVtable

The vtable payload. It patches slots in dxgi.dll's
own vtables, reached from the factory the host creates. The repository's root
CLAUDE.md applies here too; this file is only what is specific to this payload.

- **A patched slot is shared, and DXGI's own calls come through it.** dxgi.dll
  calls `GetDesc1` on every output it creates, so those get the same answer the
  host does. Measured harmless over whole sessions. `GetDesc1`,
  `CheckColorSpaceSupport` and `GetContainingOutput` log their caller, which is
  how that was established and how it stays checkable — but no hook branches
  on it, and none should.
- **Debug patches more than Release.** `Present`, `ResizeBuffers`,
  `GetContainingOutput` and `SetColorSpace1` are patched only to forward and
  log. The `CreateDXGIFactory1/2` import patch is in both builds: it is how the
  payload reaches the vtables at all.

## Do not retry these

- **Telling DXGI's calls from the host's at run time**, to withhold the lie
  from them. It costs a test on the hottest path in the DLL to change nothing
  that has ever been measured: the root's per-caller counts are what the lie
  reaches, and the videos are correct with all of it reached.
