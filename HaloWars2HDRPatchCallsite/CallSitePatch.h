// CallSitePatch.h - rewriting the host's own virtual call sites.

#pragma once

#include "Hooks.h"

namespace hw2hdr
{

// The host's own call site is rewritten, not the vtable behind it: a vtable
// belongs to dxgi.dll and is shared with its internal calls, while a call site
// belongs to one caller. Install-time only.
enum class CallSiteOutcome
{
    Patched,
    NotFound,
    Ambiguous,
    OutOfReach,
    ProtectFailed,
};

struct VirtualCall
{
    size_t slot;            // vtable index the host calls through
    size_t anchorSlot;      // another slot used nearby, to tell two sites apart
    const void* nearSite;   // search around this address only, or null for the
                            // whole image
    const IID* anchorIid;   // the interface the host asks for beside the call,
                            // which is what actually identifies it
};

// replacement must have the method's own signature; site reports where it
// landed, which is what a later nearSite is.
CallSiteOutcome PatchVirtualCall(const VirtualCall& call, void* replacement,
                                 const void** site);
const wchar_t* Describe(CallSiteOutcome outcome);

}  // namespace hw2hdr
