// CallSitePatch.h - rewriting the host's own virtual call sites.

#pragma once

#include "Hooks.h"

namespace hw2hdr
{

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
    size_t slot;
    size_t anchorSlot;
    const void* nearSite;
    const IID* anchorIid;
};

CallSiteOutcome PatchVirtualCall(const VirtualCall& call, void* replacement,
                                 const void** site);
const wchar_t* Describe(CallSiteOutcome outcome);

}  // namespace hw2hdr
