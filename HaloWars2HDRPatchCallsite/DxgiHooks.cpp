// DxgiHooks.cpp - the DXGI descriptor lie, and the gate that scopes it.
//
// Invariant: with the gate shut every hook returns the real object's answer
// untouched, and no hook initiates a DXGI call of its own.
//
// Nothing of DXGI's is modified: the host's own call sites are redirected
// instead, so only its calls arrive here. See CallSitePatch.cpp.

#include "Hooks.h"

#include <atomic>
#include <cstdio>
#include <intrin.h>

namespace hw2hdr
{
namespace
{

// --- vtable slot indices ---------------------------------------------------
// Counted from the inheritance chain, listed so each can be re-derived.

// IUnknown 0-2, IDXGIObject 3-6, IDXGIOutput 7-18, IDXGIOutput1 19-22,
// 2: 23, 3: 24, 4: 25, 5: 26, IDXGIOutput6 27 GetDesc1.
constexpr size_t kOutputGetDesc1 = 27;

// IDXGIDeviceSubObject 7, IDXGISwapChain 8-17, IDXGISwapChain1 18-28,
// IDXGISwapChain2 29-35, IDXGISwapChain3 36 GetCurrentBackBufferIndex,
// 37 CheckColorSpaceSupport, 38 SetColorSpace1.
constexpr size_t kSwapChainGetCurrentBackBufferIndex = 36;
constexpr size_t kSwapChainCheckColorSpaceSupport = 37;
constexpr size_t kSwapChainSetColorSpace1 = 38;

std::atomic<bool> g_gateOpen(false);

bool IsGateOpen()
{
    return g_gateOpen;
}

bool IsHdr(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        || colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
        || colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
}

// --- diagnostics ----------------------------------------------------------
// Compiles out at HW2HDRPATCH_LOGGING 0, call sites and arguments both, so
// nothing handed to one of these may have a side effect. Every call is logged
// every time, per frame included: nothing here may summarise or suppress.

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

// module+offset, so a caller reads the same in every session despite ASLR. A
// call from anywhere but the redirected site means the patch landed wrongly.
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

// Whether the host re-asserts HDR after a close is only visible here.
HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* self,
                                             DXGI_COLOR_SPACE_TYPE colorSpace)
{
    const HRESULT hr = self->SetColorSpace1(colorSpace);
    Log(L"host SetColorSpace1(%d %s) from %s (0x%08X)", static_cast<int>(colorSpace),
        IsHdr(colorSpace) ? L"HDR" : L"SDR", Locate(_ReturnAddress()).text, hr);
    return hr;
}

#endif  // HW2HDRPATCH_LOGGING

// --- the two hooks ---------------------------------------------------------
// Each calls the method it replaced on the object it was handed. The vtable
// still holds DXGI's own function, so that is a plain call, not a loop.

HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGIOutput6* self, DXGI_OUTPUT_DESC1* desc)
{
    const HRESULT hr = self->GetDesc1(desc);

    // One read, so the line cannot say "shut" about a call that then took the
    // open path.
    const bool gated = IsGateOpen();
    Log(L"GetDesc1(output %p) from %s, thread %lu (0x%08X) [gate %s]", self,
        Locate(_ReturnAddress()).text, GetCurrentThreadId(), hr,
        gated ? L"open" : L"shut");


    if (FAILED(hr) || desc == nullptr || !gated)
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
    const HRESULT hr = self->CheckColorSpaceSupport(colorSpace, support);

    const bool gated = IsGateOpen();
    Log(L"CheckColorSpaceSupport(%d %s) from %s (0x%08X) [gate %s]",
        static_cast<int>(colorSpace), IsHdr(colorSpace) ? L"HDR" : L"SDR",
        Locate(_ReturnAddress()).text, hr, gated ? L"open" : L"shut");

    if (SUCCEEDED(hr) && support != nullptr && gated && IsHdr(colorSpace))
    {
        *support = 0;
    }
    return hr;
}

// --- the host's factories --------------------------------------------------
// Nothing is reached through them; a factory creation is how a display change
// reads in the log.

#if HW2HDRPATCH_LOGGING

using PfnCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PfnCreateDXGIFactory g_origCreateFactory1 = nullptr;
PfnCreateDXGIFactory2 g_origCreateFactory2 = nullptr;

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory1(riid, out);
    Log(L"host called CreateDXGIFactory1(%s) (0x%08X)", IidName(riid), hr);
    LogFlush();
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory2(flags, riid, out);
    Log(L"host called CreateDXGIFactory2(flags 0x%X, %s) (0x%08X)", flags,
        IidName(riid), hr);
    LogFlush();
    return hr;
}

void LogFactoryCreations()
{
    // Pinned, not loaded: resident without a load of our own.
    HMODULE dxgi = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &dxgi)
        || dxgi == nullptr)
    {
        Log(L"[!] dxgi.dll is not loaded in the host, so its factory calls cannot "
            L"be logged.");
        return;
    }

    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));

    if (g_origCreateFactory1 == nullptr || g_origCreateFactory2 == nullptr)
    {
        Log(L"[!] dxgi.dll did not export the expected factory entry points.");
        return;
    }

    // CreateDXGIFactory is not in this host's import table; measured, the two
    // below are the only ones it calls.
    const ImportPatch patches[] = {
        {"CreateDXGIFactory1", &HookCreateDXGIFactory1},
        {"CreateDXGIFactory2", &HookCreateDXGIFactory2},
    };

    const size_t total = PatchImports(L"dxgi.dll", patches, _countof(patches));
    Log(L"Factory logging: %zu import slot(s) redirected.", total);
}

#endif  // HW2HDRPATCH_LOGGING

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
    // Nothing else in the image asks for IDXGIOutput6. The fallback is the
    // GetCurrentBackBufferIndex call that follows the poll every frame.
    const void* getDesc1Site = nullptr;
    const VirtualCall getDesc1 = {kOutputGetDesc1,
                                  kSwapChainGetCurrentBackBufferIndex, nullptr,
                                  &__uuidof(IDXGIOutput6)};
    const CallSiteOutcome lie =
        PatchVirtualCall(getDesc1, &HookGetDesc1, &getDesc1Site);
    Log(L"IDXGIOutput6::GetDesc1 (slot %zu): %s", kOutputGetDesc1, Describe(lie));

    if (lie != CallSiteOutcome::Patched)
    {
        Log(L"[!] The descriptor is what the host decides from, so without this "
            L"call site the gate has nothing to act on and the fix will not "
            L"apply. The host is otherwise unmodified.");
        return false;
    }

    // Measured 0xD2 bytes past the poll, with SetColorSpace1 right after it.
    // The id is no help: IDXGISwapChain3 is asked for somewhere else entirely.
    const void* checkSite = nullptr;
    const VirtualCall check = {kSwapChainCheckColorSpaceSupport,
                               kSwapChainSetColorSpace1, getDesc1Site,
                               &__uuidof(IDXGISwapChain3)};
    const CallSiteOutcome support =
        PatchVirtualCall(check, &HookCheckColorSpaceSupport, &checkSite);
    Log(L"IDXGISwapChain3::CheckColorSpaceSupport (slot %zu): %s",
        kSwapChainCheckColorSpaceSupport, Describe(support));

    if (support != CallSiteOutcome::Patched)
    {
        Log(L"[!] In SDR mode the host will see HDR support at swap-chain "
            L"creation and ask for HDR there; the descriptor lie still applies.");
    }

#if HW2HDRPATCH_LOGGING
    const VirtualCall setColorSpace = {kSwapChainSetColorSpace1, 0, checkSite,
                                       nullptr};
    const CallSiteOutcome label =
        PatchVirtualCall(setColorSpace, &HookSetColorSpace1, nullptr);
    Log(L"IDXGISwapChain3::SetColorSpace1 (slot %zu): %s", kSwapChainSetColorSpace1,
        Describe(label));

    LogFactoryCreations();
#endif

    return true;
}

}  // namespace hw2hdr
