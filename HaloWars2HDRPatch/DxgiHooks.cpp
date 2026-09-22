// DxgiHooks.cpp - the DXGI descriptor lie, and the gate that scopes it.
//
// Invariant: with the gate shut every hook returns the original's result
// untouched, and no hook initiates a DXGI call of its own. A patched slot is
// shared with dxgi.dll's own internal calls; those are logged, never lied to.
//
// The install creates no factory - it redirects the host's imports of
// CreateDXGIFactory1/2 and patches vtables from the factory the host creates.
// Creating one here would run inside the launcher's suspend window, where it
// can block on a lock a frozen thread holds.

#include "Hooks.h"

#include <atomic>
#include <cstdio>
#include <intrin.h>
#include <psapi.h>

namespace hw2hdr
{
namespace
{

// --- vtable slot indices ---------------------------------------------------
// Counted from the inheritance chain, listed so each can be re-derived.

// IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11, IDXGIFactory1 12-13,
// IDXGIFactory2 14 IsWindowedStereoEnabled, 15 CreateSwapChainForHwnd,
// 16 CreateSwapChainForCoreWindow, 17-23, 24 CreateSwapChainForComposition
constexpr size_t kFactoryCreateSwapChain = 10;
constexpr size_t kFactoryForHwnd = 15;
constexpr size_t kFactoryForCoreWindow = 16;
constexpr size_t kFactoryForComposition = 24;

// IDXGIOutput 7-18, IDXGIOutput1 19-22, 2: 23, 3: 24, 4: 25, 5: 26,
// IDXGIOutput6 27 GetDesc1.
constexpr size_t kOutputGetDesc1 = 27;

// IDXGIDeviceSubObject 7, IDXGISwapChain 8 Present, 13 ResizeBuffers,
// 15 GetContainingOutput, IDXGISwapChain1 18-28, IDXGISwapChain2 29-35,
// IDXGISwapChain3 36 GetCurrentBackBufferIndex, 37 CheckColorSpaceSupport,
// 38 SetColorSpace1.
constexpr size_t kSwapChainPresent = 8;
constexpr size_t kSwapChainResizeBuffers = 13;
constexpr size_t kSwapChainGetContainingOutput = 15;
constexpr size_t kSwapChainCheckColorSpaceSupport = 37;
constexpr size_t kSwapChainSetColorSpace1 = 38;

using PfnCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

using PfnGetDesc1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput6*, DXGI_OUTPUT_DESC1*);
using PfnCreateSwapChain =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                IDXGISwapChain**);
using PfnForHwnd =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                const DXGI_SWAP_CHAIN_DESC1*,
                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                IDXGIOutput*, IDXGISwapChain1**);
using PfnForCoreWindow =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                IDXGISwapChain1**);
using PfnForComposition =
    HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
                                const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                IDXGISwapChain1**);
using PfnCheckColorSpaceSupport =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE, UINT*);

PfnCreateDXGIFactory g_origCreateFactory1 = nullptr;
PfnCreateDXGIFactory2 g_origCreateFactory2 = nullptr;

PfnGetDesc1 g_origGetDesc1 = nullptr;
PfnCreateSwapChain g_origCreateSwapChain = nullptr;
PfnForHwnd g_origForHwnd = nullptr;
PfnForCoreWindow g_origForCoreWindow = nullptr;
PfnForComposition g_origForComposition = nullptr;
PfnCheckColorSpaceSupport g_origCheckColorSpace = nullptr;

std::atomic<bool> g_gateOpen(false);
std::atomic<bool> g_factoryPatched(false);
std::atomic<bool> g_outputPatched(false);
SRWLOCK g_patchLock = SRWLOCK_INIT;

// dxgi.dll's image, pinned at install.
const BYTE* g_dxgiBegin = nullptr;
const BYTE* g_dxgiEnd = nullptr;

bool IsGateOpen()
{
    return g_gateOpen;
}

// DXGI calls GetDesc1 on every output it creates, through the same shared
// slot. Its own calls are left alone unless one is shown to need the lie.
bool CalledFromDxgi(const void* returnAddress)
{
    const BYTE* address = static_cast<const BYTE*>(returnAddress);
    return address >= g_dxgiBegin && address < g_dxgiEnd;
}


bool IsHdr(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        || colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
        || colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
}

void** VTableOf(void* object)
{
    return *reinterpret_cast<void***>(object);
}

// Reported rather than logged here, so the slot name stays a literal in the
// caller's Log call and disappears with it in a no-logging build.
enum class PatchOutcome
{
    Patched,        // slot rewritten, *original captured
    AlreadyOurs,    // our hook was already there; idempotent re-entry
    NoObject,       // nothing to patch
    NotInModule,    // the slot does not point into dxgi.dll - wrong index
    ProtectFailed,  // VirtualProtect refused the page
};

bool HookIsInPlace(PatchOutcome outcome)
{
    return outcome == PatchOutcome::Patched || outcome == PatchOutcome::AlreadyOurs;
}

// [[maybe_unused]]: its only caller is a Log argument.
[[maybe_unused]] const wchar_t* Describe(PatchOutcome outcome)
{
    switch (outcome)
    {
        case PatchOutcome::Patched:       return L"hooked";
        case PatchOutcome::AlreadyOurs:   return L"already hooked";
        case PatchOutcome::NoObject:      return L"[!] no object to patch";
        case PatchOutcome::NotInModule:   return L"[!] slot is not inside dxgi.dll "
                                                 L"- refusing to patch, the index "
                                                 L"is wrong";
        case PatchOutcome::ProtectFailed: return L"[!] VirtualProtect refused the "
                                                 L"page";
    }
    return L"[!] unknown outcome";
}

// Verifies the slot still points into dxgi.dll, so a wrong index fails instead
// of corrupting the process. Caller holds g_patchLock: without it two threads
// can both read the original, and the second stores our own hook as "the
// original", which recurses for ever.
PatchOutcome PatchSlotLocked(void* object, size_t index, void* hook,
                             void** original)
{
    if (object == nullptr)
    {
        return PatchOutcome::NoObject;
    }

    void** vtable = VTableOf(object);
    void* current = vtable[index];
    if (current == hook)
    {
        return PatchOutcome::AlreadyOurs;
    }

    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(current), &owner);
    if (owner == nullptr || owner != GetModuleHandleW(L"dxgi.dll"))
    {
        return PatchOutcome::NotInModule;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        return PatchOutcome::ProtectFailed;
    }

    *original = current;
    vtable[index] = hook;

    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return PatchOutcome::Patched;
}

PatchOutcome PatchSlot(void* object, size_t index, void* hook, void** original)
{
    AutoSRWLockExclusive patchLock(g_patchLock);
    const PatchOutcome outcome = PatchSlotLocked(object, index, hook, original);
    return outcome;
}

// --- diagnostics ----------------------------------------------------------
// Compiles out at HW2HDRPATCH_LOGGING 0, call sites and arguments both, so
// nothing handed to one of these may have a side effect. Every call is logged
// every time, per frame included: nothing here may summarise or suppress.
//
// A patched slot is shared by every object of its class in the process, so
// these hooks see callers a wrapper handed out by our factory never would.
// The object pointer and the calling module are what tell them apart.

#if HW2HDRPATCH_LOGGING

const wchar_t* IidName(REFIID riid)
{
    if (riid == __uuidof(IUnknown))      return L"IUnknown";
    if (riid == __uuidof(IDXGIFactory))  return L"IDXGIFactory";
    if (riid == __uuidof(IDXGIFactory1)) return L"IDXGIFactory1";
    if (riid == __uuidof(IDXGIFactory2)) return L"IDXGIFactory2";
    if (riid == __uuidof(IDXGIFactory3)) return L"IDXGIFactory3";
    if (riid == __uuidof(IDXGIFactory4)) return L"IDXGIFactory4";
    if (riid == __uuidof(IDXGIFactory5)) return L"IDXGIFactory5";
    if (riid == __uuidof(IDXGIFactory6)) return L"IDXGIFactory6";
    if (riid == __uuidof(IDXGIFactory7)) return L"IDXGIFactory7";
    return L"(unrecognised)";
}

const wchar_t* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_UNKNOWN:            return L"UNKNOWN";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return L"R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return L"R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return L"R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return L"B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return L"B8G8R8A8_UNORM_SRGB";
        default:                             return L"other";
    }
}

// The result comes first: on a failure the descriptor fields that follow are
// whatever was already in the caller's struct.
void LogSwapChainDesc(const wchar_t* who, HRESULT hr,
                      const DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (desc == nullptr)
    {
        Log(L"%s (0x%08X), no descriptor", who, hr);
        return;
    }

    Log(L"%s (0x%08X): %ux%u, %s, %u buffers, flags 0x%X, swap effect %d, "
        L"alpha %d, scaling %d",
        who, hr, desc->Width, desc->Height, FormatName(desc->Format),
        desc->BufferCount, desc->Flags, static_cast<int>(desc->SwapEffect),
        static_cast<int>(desc->AlphaMode), static_cast<int>(desc->Scaling));
}

// module+offset rather than a raw address, so one caller reads the same in
// every session despite ASLR.
struct Location
{
    wchar_t text[MAX_PATH + 32];
};

Location Locate(const void* address)
{
    Location location = {};
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)
        || GetModuleFileNameW(module, path, MAX_PATH) == 0)
    {
        _snwprintf_s(location.text, _countof(location.text), _TRUNCATE, L"%p",
                     address);
        return location;
    }

    const wchar_t* slash = wcsrchr(path, L'\\');
    _snwprintf_s(location.text, _countof(location.text), _TRUNCATE, L"%s+0x%zX",
                 slash != nullptr ? slash + 1 : path,
                 static_cast<size_t>(static_cast<const BYTE*>(address)
                                     - reinterpret_cast<const BYTE*>(module)));
    return location;
}

using PfnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PfnResizeBuffers =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PfnGetContainingOutput =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, IDXGIOutput**);
using PfnSetColorSpace1 =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

PfnPresent g_origPresent = nullptr;
PfnResizeBuffers g_origResizeBuffers = nullptr;
PfnGetContainingOutput g_origGetContainingOutput = nullptr;
PfnSetColorSpace1 g_origSetColorSpace1 = nullptr;

// The per-frame reference the GetDesc1 polls are counted against.
HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT syncInterval,
                                      UINT flags)
{
    const HRESULT hr = g_origPresent(self, syncInterval, flags);
    Log(L"Present(swap chain %p, sync %u, flags 0x%X) (0x%08X)", self, syncInterval,
        flags, hr);
    LogFlush();
    return hr;
}

// Measured to follow every gate edge: R8G8B8A8_UNORM when it opens,
// R10G10B10A2_UNORM when it shuts.
HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT bufferCount,
                                            UINT width, UINT height,
                                            DXGI_FORMAT newFormat,
                                            UINT swapChainFlags)
{
    const HRESULT hr = g_origResizeBuffers(self, bufferCount, width, height,
                                           newFormat, swapChainFlags);
    Log(L"host ResizeBuffers(%u buffers, %ux%u, %s, flags 0x%X) (0x%08X)",
        bufferCount, width, height, FormatName(newFormat), swapChainFlags, hr);
    return hr;
}

// The output handed out here is what the host's GetDesc1 polls should name.
HRESULT STDMETHODCALLTYPE HookGetContainingOutput(IDXGISwapChain* self,
                                                  IDXGIOutput** output)
{
    const HRESULT hr = g_origGetContainingOutput(self, output);
    Log(L"GetContainingOutput(swap chain %p) from %s -> output %p (0x%08X)", self,
        Locate(_ReturnAddress()).text, output != nullptr ? *output : nullptr, hr);
    return hr;
}

// Whether the host re-asserts HDR after a close is only visible here.
HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* self,
                                             DXGI_COLOR_SPACE_TYPE colorSpace)
{
    const HRESULT hr = g_origSetColorSpace1(self, colorSpace);
    Log(L"host SetColorSpace1(%d %s) (0x%08X)", static_cast<int>(colorSpace),
        IsHdr(colorSpace) ? L"HDR" : L"SDR", hr);
    return hr;
}

#else  // !HW2HDRPATCH_LOGGING

#define LogSwapChainDesc(...) ((void)0)

#endif  // HW2HDRPATCH_LOGGING

// --- the two hooks ---------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGIOutput6* self, DXGI_OUTPUT_DESC1* desc)
{
    const HRESULT hr = g_origGetDesc1(self, desc);

    // One read, so the line cannot say "shut" about a call that then took the
    // open path.
    const bool gated = IsGateOpen();
    const void* const caller = _ReturnAddress();
    const bool fromDxgi = CalledFromDxgi(caller);
    Log(L"GetDesc1(output %p) from %s, thread %lu (0x%08X) [gate %s]", self,
        Locate(caller).text, GetCurrentThreadId(), hr,
        !gated ? L"shut" : fromDxgi ? L"open, DXGI's own call untouched" : L"open");

    if (FAILED(hr) || desc == nullptr || !gated || fromDxgi)
    {
        return hr;
    }

    desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    desc->BitsPerColor = 8;

    // Rec.709 primaries and D65 white, so gamut-volume checks also see SDR.
    desc->RedPrimary[0] = 0.640f;
    desc->RedPrimary[1] = 0.330f;
    desc->GreenPrimary[0] = 0.300f;
    desc->GreenPrimary[1] = 0.600f;
    desc->BluePrimary[0] = 0.150f;
    desc->BluePrimary[1] = 0.060f;
    desc->WhitePoint[0] = 0.3127f;
    desc->WhitePoint[1] = 0.3290f;

    desc->MinLuminance = 0.5f;
    desc->MaxLuminance = 270.0f;
    desc->MaxFullFrameLuminance = 270.0f;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCheckColorSpaceSupport(IDXGISwapChain3* self,
                                                     DXGI_COLOR_SPACE_TYPE colorSpace,
                                                     UINT* support)
{
    const HRESULT hr = g_origCheckColorSpace(self, colorSpace, support);

    const bool gated = IsGateOpen();
    const void* const caller = _ReturnAddress();
    const bool fromDxgi = CalledFromDxgi(caller);
    Log(L"CheckColorSpaceSupport(%d %s) from %s (0x%08X) [gate %s]",
        static_cast<int>(colorSpace), IsHdr(colorSpace) ? L"HDR" : L"SDR",
        Locate(caller).text, hr,
        !gated ? L"shut" : fromDxgi ? L"open, DXGI's own call untouched" : L"open");

    if (SUCCEEDED(hr) && support != nullptr && gated && !fromDxgi
        && IsHdr(colorSpace))
    {
        *support = 0;
    }
    return hr;
}

// --- reaching the swap chain vtable ----------------------------------------

// Defined below; AdoptHostFactory needs their addresses.
HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory*, IUnknown*,
                                              DXGI_SWAP_CHAIN_DESC*,
                                              IDXGISwapChain**);
HRESULT STDMETHODCALLTYPE HookForHwnd(IDXGIFactory2*, IUnknown*, HWND,
                                      const DXGI_SWAP_CHAIN_DESC1*,
                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                      IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookForCoreWindow(IDXGIFactory2*, IUnknown*, IUnknown*,
                                            const DXGI_SWAP_CHAIN_DESC1*,
                                            IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE HookForComposition(IDXGIFactory2*, IUnknown*,
                                             const DXGI_SWAP_CHAIN_DESC1*,
                                             IDXGIOutput*, IDXGISwapChain1**);

void PatchOutputVtable(IDXGIOutput* output)
{
    if (output == nullptr || g_outputPatched)
    {
        return;
    }

    IDXGIOutput6* output6 = nullptr;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6))) || output6 == nullptr)
    {
        return;
    }

    const PatchOutcome outcome = PatchSlot(output6, kOutputGetDesc1, &HookGetDesc1,
                                           reinterpret_cast<void**>(&g_origGetDesc1));
    Log(L"    IDXGIOutput6::GetDesc1 (slot %zu): %s", kOutputGetDesc1,
        Describe(outcome));

    if (HookIsInPlace(outcome))
    {
        g_outputPatched = true;
    }
    output6->Release();
}

void PatchOutputFromFactory(IDXGIFactory2* factory)
{
    if (factory == nullptr || g_outputPatched)
    {
        return;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND
            || adapter == nullptr)
        {
            break;
        }

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND
                || output == nullptr)
            {
                break;
            }
            PatchOutputVtable(output);
            output->Release();

            if (g_outputPatched)
            {
                break;
            }
        }
        adapter->Release();

        if (g_outputPatched)
        {
            break;
        }
    }
}

void AdoptHostFactory(IUnknown* unknown)
{
    if (unknown == nullptr)
    {
        return;
    }

    IDXGIFactory2* factory = nullptr;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory))) || factory == nullptr)
    {
        return;
    }

    if(!g_factoryPatched.exchange(true))
    {
        Log(L"Host created a DXGI factory; patching its vtable...");

        // Patch into a local first: a PatchSlot call inside a Log argument
        // would vanish with the Log in a no-logging build.
        [[maybe_unused]] const PatchOutcome createSwapChain =
            PatchSlot(factory, kFactoryCreateSwapChain, &HookCreateSwapChain,
                      reinterpret_cast<void**>(&g_origCreateSwapChain));
        Log(L"    IDXGIFactory::CreateSwapChain (slot %zu): %s",
            kFactoryCreateSwapChain, Describe(createSwapChain));

        [[maybe_unused]] const PatchOutcome forHwnd =
            PatchSlot(factory, kFactoryForHwnd, &HookForHwnd,
                      reinterpret_cast<void**>(&g_origForHwnd));
        Log(L"    IDXGIFactory2::CreateSwapChainForHwnd (slot %zu): %s",
            kFactoryForHwnd, Describe(forHwnd));

        [[maybe_unused]] const PatchOutcome forCoreWindow =
            PatchSlot(factory, kFactoryForCoreWindow, &HookForCoreWindow,
                      reinterpret_cast<void**>(&g_origForCoreWindow));
        Log(L"    IDXGIFactory2::CreateSwapChainForCoreWindow (slot %zu): %s",
            kFactoryForCoreWindow, Describe(forCoreWindow));

        [[maybe_unused]] const PatchOutcome forComposition =
            PatchSlot(factory, kFactoryForComposition, &HookForComposition,
                      reinterpret_cast<void**>(&g_origForComposition));
        Log(L"    IDXGIFactory2::CreateSwapChainForComposition (slot %zu): %s",
            kFactoryForComposition, Describe(forComposition));
    }

    PatchOutputFromFactory(factory);
    factory->Release();

    if (!g_outputPatched)
    {
        Log(L"[!] No IDXGIOutput6 reachable from the host's factory yet; will try "
            L"again from its swap chain.");
    }
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory1(riid, out);
    Log(L"host called CreateDXGIFactory1(%s) (0x%08X)", IidName(riid), hr);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(static_cast<IUnknown*>(*out));
    }
    LogFlush();
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory2(flags, riid, out);
    Log(L"host called CreateDXGIFactory2(flags 0x%X, %s) (0x%08X)", flags,
        IidName(riid), hr);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(static_cast<IUnknown*>(*out));
    }
    LogFlush();
    return hr;
}

void HookSwapChain(IDXGISwapChain1* swapChain)
{
    if (swapChain == nullptr)
    {
        return;
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
    {
        [[maybe_unused]] const PatchOutcome checkColorSpace =
            PatchSlot(sc3, kSwapChainCheckColorSpaceSupport,
                      &HookCheckColorSpaceSupport,
                      reinterpret_cast<void**>(&g_origCheckColorSpace));
        Log(L"    IDXGISwapChain3::CheckColorSpaceSupport (slot %zu): %s",
            kSwapChainCheckColorSpaceSupport, Describe(checkColorSpace));

#if HW2HDRPATCH_LOGGING
        // Diagnostics only: they forward and log, nothing more.
        const PatchOutcome setColorSpace =
            PatchSlot(sc3, kSwapChainSetColorSpace1, &HookSetColorSpace1,
                      reinterpret_cast<void**>(&g_origSetColorSpace1));
        Log(L"    IDXGISwapChain3::SetColorSpace1 (slot %zu): %s",
            kSwapChainSetColorSpace1, Describe(setColorSpace));

        const PatchOutcome present =
            PatchSlot(sc3, kSwapChainPresent, &HookPresent,
                      reinterpret_cast<void**>(&g_origPresent));
        Log(L"    IDXGISwapChain::Present (slot %zu): %s", kSwapChainPresent,
            Describe(present));

        const PatchOutcome resizeBuffers =
            PatchSlot(sc3, kSwapChainResizeBuffers, &HookResizeBuffers,
                      reinterpret_cast<void**>(&g_origResizeBuffers));
        Log(L"    IDXGISwapChain::ResizeBuffers (slot %zu): %s",
            kSwapChainResizeBuffers, Describe(resizeBuffers));

        const PatchOutcome getContainingOutput =
            PatchSlot(sc3, kSwapChainGetContainingOutput, &HookGetContainingOutput,
                      reinterpret_cast<void**>(&g_origGetContainingOutput));
        Log(L"    IDXGISwapChain::GetContainingOutput (slot %zu): %s",
            kSwapChainGetContainingOutput, Describe(getContainingOutput));
#endif

        sc3->Release();
    }

    // Second chance, for a factory that had no outputs to enumerate.
    if (!g_outputPatched)
    {
        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(swapChain->GetContainingOutput(&output)) && output != nullptr)
        {
            PatchOutputVtable(output);
            output->Release();
        }
    }
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out)
{
    const HRESULT hr = g_origCreateSwapChain(self, device, desc, out);
    Log(L"host CreateSwapChain (0x%08X): %s, %u buffers, flags 0x%X", hr,
        desc != nullptr ? FormatName(desc->BufferDesc.Format) : L"(no descriptor)",
        desc != nullptr ? desc->BufferCount : 0, desc != nullptr ? desc->Flags : 0);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr)
        {
            HookSwapChain(sc1);
            sc1->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookForHwnd(IDXGIFactory2* self, IUnknown* device,
                                      HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                      IDXGIOutput* restrictTo,
                                      IDXGISwapChain1** out)
{
    const HRESULT hr =
        g_origForHwnd(self, device, hwnd, desc, fullscreen, restrictTo, out);
    LogSwapChainDesc(L"host CreateSwapChainForHwnd", hr, desc);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        HookSwapChain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookForCoreWindow(IDXGIFactory2* self, IUnknown* device,
                                            IUnknown* window,
                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                            IDXGIOutput* restrictTo,
                                            IDXGISwapChain1** out)
{
    const HRESULT hr =
        g_origForCoreWindow(self, device, window, desc, restrictTo, out);
    LogSwapChainDesc(L"host CreateSwapChainForCoreWindow", hr, desc);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        HookSwapChain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookForComposition(IDXGIFactory2* self, IUnknown* device,
                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                             IDXGIOutput* restrictTo,
                                             IDXGISwapChain1** out)
{
    const HRESULT hr = g_origForComposition(self, device, desc, restrictTo, out);
    LogSwapChainDesc(L"host CreateSwapChainForComposition", hr, desc);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        HookSwapChain(*out);
    }
    return hr;
}

}  // namespace

bool OpenGate()
{
    const bool moved = !g_gateOpen.exchange(true);
    Log(L"==== gate %s ====", moved ? L"OPEN" : L"already open");
    return moved;
}

bool CloseGate()
{
    const bool moved = g_gateOpen.exchange(false);
    Log(L"==== gate %s ====", moved ? L"SHUT" : L"already shut");
    return moved;
}

bool InstallDxgiHooks()
{
    // Pinned, not loaded: resident for the life of the process without causing
    // a load of our own.
    HMODULE dxgi = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &dxgi)
        || dxgi == nullptr)
    {
        Log(L"[!] dxgi.dll is not loaded in the host - nothing to hook.");
        return false;
    }

    MODULEINFO image = {};
    if (!GetModuleInformation(GetCurrentProcess(), dxgi, &image, sizeof(image)))
    {
        Log(L"[!] dxgi.dll's image range is unknown (%lu), so its own calls could "
            L"not be told from the host's.", GetLastError());
        return false;
    }
    g_dxgiBegin = static_cast<const BYTE*>(image.lpBaseOfDll);
    g_dxgiEnd = g_dxgiBegin + image.SizeOfImage;

    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));

    if (g_origCreateFactory1 == nullptr || g_origCreateFactory2 == nullptr)
    {
        Log(L"[!] dxgi.dll did not export the expected factory entry points.");
        return false;
    }

    // CreateDXGIFactory is not in this host's import table; measured, the two
    // below are the only ones it calls.
    const ImportPatch patches[] = {
        {"CreateDXGIFactory1", &HookCreateDXGIFactory1},
        {"CreateDXGIFactory2", &HookCreateDXGIFactory2},
    };

    const size_t total = PatchImports(L"dxgi.dll", patches, _countof(patches));
    if (total == 0)
    {
        Log(L"[!] Nothing imports CreateDXGIFactory* from dxgi.dll by name. The "
            L"host resolves it dynamically, which an import-table patch cannot "
            L"reach, so no DXGI hook will ever be installed and the fix will not "
            L"apply. This needs a different interception point.");
        return false;
    }

    Log(L"Waiting for the host to create its DXGI factory (%zu import slot(s) "
        L"redirected).", total);
    return true;
}

}  // namespace hw2hdr
