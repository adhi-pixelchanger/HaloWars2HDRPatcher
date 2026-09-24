# HaloWars2HDRPatchQI

The wrapper payload. It hands the host proxy objects — a factory, and the swap
chain and output reached through it — each wrapping at the newest interface
level. Nothing of DXGI's is written, and DXGI's internal calls reach its real
objects, never a proxy. The repository's root CLAUDE.md applies here too; this
file is only what is specific to this payload.

- **Every `Wrap*` helper lands its result in a `[[maybe_unused]] const bool`
  before logging it.** `Log(L"%s", Wrap<OutputProxy, ...>(output) ? ... : ...)`
  would silently stop wrapping in Release, where the whole `Log` call vanishes.
- **An interface a proxy does not answer loses the object from then on**: the
  host holds the real one after that. The Debug log marks every such
  `QueryInterface` with `[!]`, and none remain on the host's path.
- **What the host asks each object for**, measured: the factory for
  `IDXGIFactory`, `1`, `4`, `5`, never 2, 3, 6 or 7; the swap chain for
  `IDXGISwapChain1`, `3`, `4`, never 2; the output for `IDXGIOutput6`, 4602
  times a session. `GetDesc1` exists only on `IDXGIOutput6`, and the level a
  wrapper can wrap at is a property of the OS's DXGI, not of the game's 2017
  vintage.
- **Adapters are not wrapped.** D3D12 asks each for `IDXGICoreAdapter`
  (`{F0DB4C7F-FE5A-42A2-BD62-F2A6CF6FC83E}`) and
  `{23FDBE30-0292-491B-89F2-AEECFCFA11C6}` — not RenderDoc, which is not
  installed on the testing machine.

## Do not retry these

- **Wrapping the adapter.** Its only use would be wrapping `EnumOutputs`
  results, and the host never polls `GetDesc1` on those — the 1:1 count between
  `GetContainingOutput` and `GetDesc1` is the proof. The adapter is also the
  only object anything asks for an interface we do not answer.
- **Unwrapping an adapter for `D3D12CreateDevice`.** D3D12 accepts a proxy
  adapter: with one wrapped, it asked for its private interfaces, got the real
  object through the fall-through, created the device and rendered 4600 frames.
