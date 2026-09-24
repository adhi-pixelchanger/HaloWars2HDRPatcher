# HaloWars2HDRPatchCallsite

The call-site payload. It rewrites the host's own call instructions, so nothing
of DXGI's is modified — not its image, not its vtables — and DXGI's internal
calls never reach a hook. The repository's root CLAUDE.md applies here too;
this file is only what is specific to this payload.

## What the call-site patch depends on

- **The launcher freezes every game thread for the install**, which is the only
  reason rewriting an instruction in the host is safe. That holds for the
  default argument route only: on the environment route `DllMain` installs on
  its own thread and the launcher resumes the game without waiting, so there
  the rewrite races the host's startup. Nothing may move the patch later.
- **A slot below 16 is reached with an 8-bit displacement**, three bytes, and
  the replacement needs six. `Present` (8), `ResizeBuffers` (13) and
  `GetContainingOutput` (15) therefore cannot be redirected in place and are
  not logged. `GetDesc1` (27), `CheckColorSpaceSupport` (37) and
  `SetColorSpace1` (38) are far enough in to use a 32-bit displacement.
- **Control Flow Guard changes which instruction is patched.** A CFG build
  loads the slot and calls through the guard dispatcher, so the load is what
  gets redirected; the dispatcher accepts our target because this DLL is not a
  CFG image. The launcher's mitigation report prints whether the host has CFG.
- **Arbitrary Code Guard would block it**, since it refuses to make a code page
  writable. The launcher reports ACG for that reason, and the payload logs a
  refusal rather than failing quietly.
- **A displacement identifies nothing.** Measured: **244** call sites reach
  `+0xD8` (1053 before ordinary stack loads were excluded), **139** reach
  `+0x128`, **57** reach `+0x130`. Requiring the `GetCurrentBackBufferIndex`
  slot within 0x100 bytes left 30 of the first set, and 7 of the second.
- **Two tests do identify one, and they are what the patcher uses.** The
  interface id: only the code that asks an object for `IDXGIOutput6` goes on to
  call `GetDesc1` on it, which took 244 sites to exactly one, `+0x8186D`. Then
  proximity: a call the host makes beside one already redirected. The id says
  nothing about `CheckColorSpaceSupport` — the host asks for `IDXGISwapChain3`
  somewhere else entirely — but its site is `+0x8193F`, 0xD2 bytes past where
  the descriptor is polled, and `SetColorSpace1` follows that. The slot anchor
  is the last fallback.
- **The load form is only looked for in a CFG host.** `48 8B 95 D8 00 00 00` is
  `mov rdx,[rbp+0D8h]`, an ordinary stack load, and the first scan matched
  several of those. It also has to be followed by an indirect call.
- **Scanning costs install time, and the game is frozen for it.** A scan per
  slot measured 3 s for three slots; finding the other two beside the first
  took the whole install to **0.97 s**.
- **The three sites, measured.** `GetDesc1` at `exe+0x8186D`
  (`FF 90 D8 00 00 00`, `call qword ptr [rax+0D8h]`), `CheckColorSpaceSupport`
  at `+0x8193F`, `SetColorSpace1` at `+0x8195A`. Each ends at its logged
  return address: `+0x81873` and `+0x81945` from the Vtable payload,
  `+0x81960` from this one. Game version 1.11.2931.2, and no address is
  compiled in.
