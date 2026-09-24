// CallSitePatch.cpp - redirecting the host's own virtual call sites.
//
// A vtable is shared with the calls the defining DLL makes itself; a call site
// belongs to one caller. Rewriting the site leaves dxgi.dll untouched.
//
// Two encodings, each replaced by one of its own length:
//
//   call qword ptr [rax+0D8h]       FF 90 D8 00 00 00
//   call qword ptr [rip+disp]       FF 15 <disp32>
//
//   mov rax, qword ptr [rax+0D8h]   48 8B 80 D8 00 00 00
//   mov rax, qword ptr [rip+disp]   48 8B 05 <disp32>
//
// The second is a Control Flow Guard build, which calls through a dispatcher
// that accepts our target because this DLL is not a CFG image. It is only
// looked for in a CFG host, since it is also what a stack load looks like. A
// slot below 16 uses an 8-bit displacement, three bytes, too short to replace.
//
// A displacement identifies nothing - the host reaches +0xD8 from 244 sites.
// The interface id does: only the code that asks for IDXGIOutput6 goes on to
// call GetDesc1 on it.
//
// Both replacements read the target from a page within 2GB, as far as a
// RIP-relative operand reaches; no memory is made executable. Install time
// only, with the game's threads frozen.

#include "CallSitePatch.h"

#include <cstring>

namespace hw2hdr
{
namespace
{

constexpr size_t kAnchorWindow = 0x100;
constexpr size_t kSearchWindow = 0x400;
constexpr size_t kSlotPageSize = 4096;
constexpr size_t kMaxCandidates = 8;
constexpr size_t kMaxReferences = 16;
constexpr ptrdiff_t kRipReach = 0x7FFFFFFF;

BYTE* g_slotPage = nullptr;
size_t g_slotPageUsed = 0;

struct Range
{
    BYTE* begin;
    BYTE* end;
};

struct Site
{
    BYTE* address;
    size_t length;
    BYTE rex;
    BYTE opcode;
    BYTE reg;
};

BYTE* HostBase()
{
    return reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
}

const IMAGE_NT_HEADERS* HostHeaders()
{
    BYTE* const base = HostBase();
    if (base == nullptr)
    {
        return nullptr;
    }

    auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return nullptr;
    }

    auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

// The host's code, or the readable rest where its constants live.
size_t HostSections(bool executable, Range* ranges, size_t capacity)
{
    const IMAGE_NT_HEADERS* const nt = HostHeaders();
    if (nt == nullptr)
    {
        return 0;
    }

    size_t found = 0;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections && found < capacity;
         ++index, ++section)
    {
        const bool isCode = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool isReadable = (section->Characteristics & IMAGE_SCN_MEM_READ) != 0;
        if (isCode != executable || !isReadable || section->Misc.VirtualSize == 0)
        {
            continue;
        }

        ranges[found].begin = HostBase() + section->VirtualAddress;
        ranges[found].end = ranges[found].begin + section->Misc.VirtualSize;
        ++found;
    }
    return found;
}

bool HostUsesControlFlowGuard()
{
    const IMAGE_NT_HEADERS* const nt = HostHeaders();
    return nt != nullptr
        && (nt->OptionalHeader.DllCharacteristics
            & IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;
}

bool AllocateSlotPage()
{
    if (g_slotPage != nullptr)
    {
        return true;
    }

    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    BYTE* const base = HostBase();
    const size_t granularity = system.dwAllocationGranularity;

    for (size_t distance = granularity; distance < 0x30000000; distance += granularity)
    {
        BYTE* const below = base - distance;
        BYTE* const above = base + distance;
        for (int direction = 0; direction < 2; ++direction)
        {
            BYTE* const candidate = direction == 0 ? below : above;
            if (candidate < system.lpMinimumApplicationAddress
                || candidate > system.lpMaximumApplicationAddress)
            {
                continue;
            }

            void* const page = VirtualAlloc(candidate, kSlotPageSize,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (page != nullptr)
            {
                g_slotPage = static_cast<BYTE*>(page);
                return true;
            }
        }
    }
    return false;
}

void** ReserveSlot(void* value)
{
    if (!AllocateSlotPage() || g_slotPageUsed + sizeof(void*) > kSlotPageSize)
    {
        return nullptr;
    }

    auto** const slot = reinterpret_cast<void**>(g_slotPage + g_slotPageUsed);
    g_slotPageUsed += sizeof(void*);
    *slot = value;
    return slot;
}

// How a guarded load is spent.
bool IsIndirectCall(const BYTE* address, const BYTE* end)
{
    if (end - address < 2)
    {
        return false;
    }

    const size_t index = (address[0] & 0xF0) == 0x40 ? 1u : 0u;
    if (address[index] != 0xFF)
    {
        return false;
    }

    const BYTE modrm = address[index + 1];
    return ((modrm >> 3) & 0x07) == 2;  // /2, whatever it reads the target from
}

// Matches a whole instruction or nothing: a partial match is never rewritten.
bool MatchSite(BYTE* address, const BYTE* end, UINT32 displacement, Site* site)
{
    if (end - address < 8)
    {
        return false;
    }

    size_t index = 0;
    BYTE rex = 0;
    if ((address[0] & 0xF0) == 0x40)
    {
        rex = address[0];
        index = 1;
    }

    const BYTE opcode = address[index];
    if (opcode != 0xFF && opcode != 0x8B)
    {
        return false;
    }

    const BYTE modrm = address[index + 1];
    if ((modrm & 0xC0) != 0x80 || (modrm & 0x07) == 0x04)
    {
        return false;  // not the 32-bit displacement form, or a SIB byte follows
    }

    UINT32 found = 0;
    memcpy(&found, address + index + 2, sizeof(found));
    if (found != displacement)
    {
        return false;
    }

    const BYTE reg = static_cast<BYTE>((modrm >> 3) & 0x07);
    const size_t length = index + 2 + sizeof(UINT32);
    if (opcode == 0xFF && reg != 2)
    {
        return false;  // /2 is the indirect call; anything else is another op
    }
    if (opcode == 0x8B)
    {
        // Only a CFG host loads a slot before calling it; anywhere else this
        // is an ordinary load.
        if ((rex & 0x08) == 0 || !HostUsesControlFlowGuard()
            || !IsIndirectCall(address + length, end))
        {
            return false;
        }
    }

    site->address = address;
    site->length = length;
    site->rex = rex;
    site->opcode = opcode;
    site->reg = reg;
    return true;
}

bool HasAnchor(const Site& site, const Range& range, UINT32 displacement)
{
    BYTE* const begin =
        site.address - range.begin > static_cast<ptrdiff_t>(kAnchorWindow)
            ? site.address - kAnchorWindow
            : range.begin;
    BYTE* const end = range.end - site.address > static_cast<ptrdiff_t>(kAnchorWindow)
                          ? site.address + kAnchorWindow
                          : range.end;

    for (BYTE* address = begin; address + 8 <= end; ++address)
    {
        Site anchor = {};
        if (MatchSite(address, end, displacement, &anchor))
        {
            return true;
        }
    }
    return false;
}

// All counted, kept while there is room. An anchor keeps only the sites with
// that slot nearby.
size_t FindSites(const Range& range, UINT32 displacement, UINT32 anchor, Site* sites,
                 size_t capacity)
{
    size_t found = 0;
    for (BYTE* address = range.begin; address + 8 <= range.end; ++address)
    {
        Site site = {};
        if (!MatchSite(address, range.end, displacement, &site))
        {
            continue;
        }
        address += site.length - 1;

        if (anchor != 0 && !HasAnchor(site, range, anchor))
        {
            continue;
        }

        if (found < capacity)
        {
            sites[found] = site;
        }
        ++found;
    }
    return found;
}

// The same across several ranges.
size_t Gather(const Range* ranges, size_t rangeCount, UINT32 displacement,
              UINT32 anchor, Site* sites)
{
    size_t found = 0;
    for (size_t index = 0; index < rangeCount; ++index)
    {
        const size_t kept = found < kMaxCandidates ? found : kMaxCandidates;
        found += FindSites(ranges[index], displacement, anchor, sites + kept,
                           kMaxCandidates - kept);
    }
    return found;
}

// Where the host keeps this id; there may be more than one copy.
size_t FindIid(const IID& iid, BYTE** addresses, size_t capacity)
{
    Range data[16] = {};
    const size_t dataCount = HostSections(false, data, _countof(data));

    size_t found = 0;
    for (size_t index = 0; index < dataCount && found < capacity; ++index)
    {
        for (BYTE* address = data[index].begin;
             address + sizeof(IID) <= data[index].end && found < capacity;
             address += sizeof(UINT32))
        {
            if (memcmp(address, &iid, sizeof(IID)) == 0)
            {
                addresses[found] = address;
                ++found;
            }
        }
    }
    return found;
}

// lea reg,[rip+disp32], how the host names an id.
bool PointsAt(const BYTE* address, const BYTE* end, BYTE* const* targets,
              size_t targetCount)
{
    if (end - address < 7 || (address[0] & 0xF8) != 0x48 || address[1] != 0x8D)
    {
        return false;
    }

    const BYTE modrm = address[2];
    if ((modrm & 0xC7) != 0x05)  // mod 00, rm 101: RIP-relative
    {
        return false;
    }

    INT32 displacement = 0;
    memcpy(&displacement, address + 3, sizeof(displacement));
    const BYTE* const target = address + 7 + displacement;

    for (size_t index = 0; index < targetCount; ++index)
    {
        if (target == targets[index])
        {
            return true;
        }
    }
    return false;
}

// Sites within reach of an instruction naming the id.
size_t GatherNearIid(const Range* ranges, size_t rangeCount, UINT32 displacement,
                     const IID& iid, Site* sites)
{
    BYTE* targets[8] = {};
    const size_t targetCount = FindIid(iid, targets, _countof(targets));
    if (targetCount == 0)
    {
        return 0;
    }

    size_t found = 0;
    for (size_t index = 0; index < rangeCount; ++index)
    {
        const Range& range = ranges[index];
        for (BYTE* address = range.begin; address + 7 <= range.end; ++address)
        {
            if (!PointsAt(address, range.end, targets, targetCount))
            {
                continue;
            }

            const Range window = {
                address - range.begin > static_cast<ptrdiff_t>(kAnchorWindow)
                    ? address - kAnchorWindow
                    : range.begin,
                range.end - address > static_cast<ptrdiff_t>(kAnchorWindow)
                    ? address + kAnchorWindow
                    : range.end,
            };

            Site nearby[kMaxCandidates] = {};
            const size_t count =
                FindSites(window, displacement, 0, nearby, _countof(nearby));
            for (size_t site = 0; site < count && site < _countof(nearby); ++site)
            {
                bool seen = false;
                for (size_t kept = 0; kept < found && kept < kMaxCandidates; ++kept)
                {
                    seen = seen || sites[kept].address == nearby[site].address;
                }
                if (seen)
                {
                    continue;
                }

                if (found < kMaxCandidates)
                {
                    sites[found] = nearby[site];
                }
                ++found;
            }
        }
    }
    return found;
}

// The code around a site already found.
bool WindowAround(const void* centre, const Range* ranges, size_t rangeCount,
                  Range* window)
{
    auto* const address = static_cast<BYTE*>(const_cast<void*>(centre));
    for (size_t index = 0; index < rangeCount; ++index)
    {
        if (address < ranges[index].begin || address >= ranges[index].end)
        {
            continue;
        }

        window->begin =
            address - ranges[index].begin > static_cast<ptrdiff_t>(kSearchWindow)
                ? address - kSearchWindow
                : ranges[index].begin;
        window->end =
            ranges[index].end - address > static_cast<ptrdiff_t>(kSearchWindow)
                ? address + kSearchWindow
                : ranges[index].end;
        return true;
    }
    return false;
}

CallSiteOutcome WriteRedirect(const Site& site, void* replacement)
{
    void** const slot = ReserveSlot(replacement);
    if (slot == nullptr)
    {
        return CallSiteOutcome::OutOfReach;
    }

    BYTE patch[8] = {};
    size_t length = 0;
    if (site.opcode == 0xFF)
    {
        patch[0] = 0xFF;
        patch[1] = 0x15;
        length = 6;
    }
    else
    {
        patch[0] = site.rex;
        patch[1] = 0x8B;
        patch[2] = static_cast<BYTE>(0x05 | (site.reg << 3));
        length = 7;
    }

    const ptrdiff_t relative = reinterpret_cast<BYTE*>(slot) - (site.address + length);
    if (relative > kRipReach || relative < -kRipReach)
    {
        return CallSiteOutcome::OutOfReach;
    }

    const INT32 displacement = static_cast<INT32>(relative);
    memcpy(patch + length - sizeof(displacement), &displacement, sizeof(displacement));
    for (size_t index = length; index < site.length; ++index)
    {
        patch[index] = 0x90;  // the spare byte a shorter replacement leaves
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return CallSiteOutcome::ProtectFailed;
    }

    memcpy(site.address, patch, site.length);

    VirtualProtect(site.address, site.length, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
    return CallSiteOutcome::Patched;
}

#if HW2HDRPATCH_LOGGING

void LogSites(const wchar_t* what, size_t slot, size_t found, const Site* sites)
{
    Log(L"    slot %zu: %zu %s", slot, found, what);
    for (size_t index = 0; index < found && index < kMaxCandidates; ++index)
    {
        const BYTE* const bytes = sites[index].address;
        Log(L"        +0x%zX %s: %02X %02X %02X %02X %02X %02X %02X",
            static_cast<size_t>(bytes - HostBase()),
            sites[index].opcode == 0xFF ? L"call" : L"guarded load", bytes[0],
            bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6]);
    }
}

// What is at that displacement, so an unknown encoding reads as itself.
void LogDisplacementUses(const Range* ranges, size_t rangeCount, UINT32 displacement)
{
    size_t listed = 0;
    size_t total = 0;

    for (size_t index = 0; index < rangeCount; ++index)
    {
        for (BYTE* address = ranges[index].begin; address + 8 <= ranges[index].end;
             ++address)
        {
            const size_t offset = (address[0] & 0xF0) == 0x40 ? 1u : 0u;
            const BYTE modrm = address[offset + 1];
            if ((modrm & 0xC0) != 0x80 || (modrm & 0x07) == 0x04)
            {
                continue;
            }

            UINT32 found = 0;
            memcpy(&found, address + offset + 2, sizeof(found));
            if (found != displacement)
            {
                continue;
            }

            ++total;
            if (listed < kMaxReferences)
            {
                Log(L"        +0x%zX: %02X %02X %02X %02X %02X %02X %02X %02X",
                    static_cast<size_t>(address - HostBase()), address[0], address[1],
                    address[2], address[3], address[4], address[5], address[6],
                    address[7]);
                ++listed;
            }
        }
    }

    Log(L"        %zu instruction(s) reach +0x%X in all; the %zu above are the "
        L"first of them", total, displacement, listed);
}

#else  // !HW2HDRPATCH_LOGGING

#define LogSites(...) ((void)0)
#define LogDisplacementUses(...) ((void)0)

#endif  // HW2HDRPATCH_LOGGING

}  // namespace

const wchar_t* Describe(CallSiteOutcome outcome)
{
    switch (outcome)
    {
        case CallSiteOutcome::Patched:       return L"redirected";
        case CallSiteOutcome::NotFound:      return L"[!] no call site found";
        case CallSiteOutcome::Ambiguous:     return L"[!] several call sites, none "
                                                    L"of them distinguishable";
        case CallSiteOutcome::OutOfReach:    return L"[!] no writable page within "
                                                    L"reach of the call site";
        case CallSiteOutcome::ProtectFailed: return L"[!] VirtualProtect refused "
                                                    L"the page";
    }
    return L"[!] unknown outcome";
}

CallSiteOutcome PatchVirtualCall(const VirtualCall& call, void* replacement,
                                 const void** site)
{
    const UINT32 displacement = static_cast<UINT32>(call.slot * sizeof(void*));

    Range ranges[8] = {};
    size_t rangeCount = HostSections(true, ranges, _countof(ranges));
    if (rangeCount == 0)
    {
        return CallSiteOutcome::NotFound;
    }

    // Beside a site already redirected: the sharpest test and the cheapest.
    // Nothing there only means this build put the two apart.
    Range window = {};
    if (call.nearSite != nullptr && WindowAround(call.nearSite, ranges, rangeCount,
                                                 &window))
    {
        Site beside[kMaxCandidates] = {};
        const size_t found = Gather(&window, 1, displacement, 0, beside);
        LogSites(L"call site(s) beside the last one", call.slot, found, beside);

        if (found == 1)
        {
            const CallSiteOutcome outcome = WriteRedirect(beside[0], replacement);
            if (outcome == CallSiteOutcome::Patched && site != nullptr)
            {
                *site = beside[0].address;
            }
            return outcome;
        }
    }

    Site candidates[kMaxCandidates] = {};
    size_t found = Gather(ranges, rangeCount, displacement, 0, candidates);
    LogSites(L"call site(s)", call.slot, found, candidates);

    if (found == 0)
    {
        LogDisplacementUses(ranges, rangeCount, displacement);
        return CallSiteOutcome::NotFound;
    }

    // The id first, the slot anchor as the weaker fallback.
    if (found > 1 && call.anchorIid != nullptr)
    {
        Site beside[kMaxCandidates] = {};
        const size_t narrowed =
            GatherNearIid(ranges, rangeCount, displacement, *call.anchorIid, beside);
        LogSites(L"of them beside the interface id", call.slot, narrowed, beside);

        // An id named somewhere else entirely narrows nothing.
        if (narrowed > 0)
        {
            memcpy(candidates, beside, sizeof(candidates));
            found = narrowed;
        }
    }

    if (found > 1 && call.anchorSlot != 0)
    {
        const UINT32 anchor = static_cast<UINT32>(call.anchorSlot * sizeof(void*));
        Site anchored[kMaxCandidates] = {};
        const size_t narrowed =
            Gather(ranges, rangeCount, displacement, anchor, anchored);
        LogSites(L"of them with the anchor slot nearby", call.slot, narrowed,
                 anchored);

        if (narrowed > 0)
        {
            memcpy(candidates, anchored, sizeof(candidates));
            found = narrowed;
        }
    }

    if (found != 1)
    {
        return found == 0 ? CallSiteOutcome::NotFound : CallSiteOutcome::Ambiguous;
    }

    const CallSiteOutcome outcome = WriteRedirect(candidates[0], replacement);
    if (outcome == CallSiteOutcome::Patched && site != nullptr)
    {
        *site = candidates[0].address;
    }
    return outcome;
}

}  // namespace hw2hdr
