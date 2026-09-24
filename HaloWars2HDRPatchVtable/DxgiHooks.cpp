// DxgiHooks.cpp - the DXGI descriptor lie, and the gate that scopes it.
//
// Invariant: with the gate shut every hook returns the original's result
// untouched; a hook's only DXGI calls of its own reach what it patches.
//
// The install creates no factory: it redirects the host's CreateDXGIFactory1/2
// imports and patches vtables from the factory the host creates. Creating one
// here would run inside the launcher's suspend window.

#include "Hooks.h"

#include <atomic>
#include <cstdio>
#include <intrin.h>

namespace hw2hdr
{
namespace
{

// --- vtable slots, counted from the inheritance chain ----------------------

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

void** VTableOf(void* object)
{
    return *reinterpret_cast<void***>(object);
}

enum class PatchOutcome
{
    Patched,
    AlreadyOurs,
    NoObject,
    NotInModule,    // the slot does not point into dxgi.dll - wrong index
    ProtectFailed,
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
        case PatchOutcome::NoObject:      return L"[!] no object";
        case PatchOutcome::NotInModule:   return L"[!] not a dxgi.dll slot";
        case PatchOutcome::ProtectFailed: return L"[!] VirtualProtect failed";
    }
    return L"[!] unknown outcome";
}

// A slot outside dxgi.dll means a wrong index, so it is refused. Caller holds
// g_patchLock, or two threads can capture our own hook as the original.
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

// --- diagnostics, Debug only -----------------------------------------------

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

void LogSwapChainDesc(const wchar_t* who, HRESULT hr,
                      const DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (desc == nullptr)
    {
        Log(L"%s (0x%08X)", who, hr);
        return;
    }

    Log(L"%s (0x%08X): %ux%u, %s, %u buffers, flags 0x%X, swap effect %d",
        who, hr, desc->Width, desc->Height, FormatName(desc->Format),
        desc->BufferCount, desc->Flags, static_cast<int>(desc->SwapEffect));
}

// module+offset, so a caller reads the same in every session despite ASLR.
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

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT syncInterval,
                                      UINT flags)
{
    const HRESULT hr = g_origPresent(self, syncInterval, flags);
    Log(L"Present(sync %u, flags 0x%X) (0x%08X)", syncInterval, flags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT bufferCount,
                                            UINT width, UINT height,
                                            DXGI_FORMAT newFormat,
                                            UINT swapChainFlags)
{
    const HRESULT hr = g_origResizeBuffers(self, bufferCount, width, height,
                                           newFormat, swapChainFlags);
    Log(L"ResizeBuffers(%u, %ux%u, %s, flags 0x%X) (0x%08X)", bufferCount, width,
        height, FormatName(newFormat), swapChainFlags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetContainingOutput(IDXGISwapChain* self,
                                                  IDXGIOutput** output)
{
    const HRESULT hr = g_origGetContainingOutput(self, output);
    Log(L"GetContainingOutput from %s -> %p (0x%08X)", Locate(_ReturnAddress()).text,
        output != nullptr ? *output : nullptr, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* self,
                                             DXGI_COLOR_SPACE_TYPE colorSpace)
{
    const HRESULT hr = g_origSetColorSpace1(self, colorSpace);
    Log(L"SetColorSpace1(%d %s) (0x%08X)", static_cast<int>(colorSpace),
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

    // Read once, so the log and the lie agree.
    const bool gated = IsGateOpen();
    Log(L"GetDesc1(%p) from %s, thread %lu (0x%08X) [gate %s]", self,
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
    const HRESULT hr = g_origCheckColorSpace(self, colorSpace, support);

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

// --- reaching the swap chain and output vtables ----------------------------

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
    Log(L"    IDXGIOutput6::GetDesc1: %s", Describe(outcome));

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

    if (!g_factoryPatched.exchange(true))
    {
        [[maybe_unused]] const PatchOutcome createSwapChain =
            PatchSlot(factory, kFactoryCreateSwapChain, &HookCreateSwapChain,
                      reinterpret_cast<void**>(&g_origCreateSwapChain));
        Log(L"    IDXGIFactory::CreateSwapChain: %s", Describe(createSwapChain));

        [[maybe_unused]] const PatchOutcome forHwnd =
            PatchSlot(factory, kFactoryForHwnd, &HookForHwnd,
                      reinterpret_cast<void**>(&g_origForHwnd));
        Log(L"    IDXGIFactory2::CreateSwapChainForHwnd: %s", Describe(forHwnd));

        [[maybe_unused]] const PatchOutcome forCoreWindow =
            PatchSlot(factory, kFactoryForCoreWindow, &HookForCoreWindow,
                      reinterpret_cast<void**>(&g_origForCoreWindow));
        Log(L"    IDXGIFactory2::CreateSwapChainForCoreWindow: %s",
            Describe(forCoreWindow));

        [[maybe_unused]] const PatchOutcome forComposition =
            PatchSlot(factory, kFactoryForComposition, &HookForComposition,
                      reinterpret_cast<void**>(&g_origForComposition));
        Log(L"    IDXGIFactory2::CreateSwapChainForComposition: %s",
            Describe(forComposition));
    }

    PatchOutputFromFactory(factory);
    factory->Release();
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory1(riid, out);
    Log(L"CreateDXGIFactory1(%s) (0x%08X)", IidName(riid), hr);
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
    Log(L"CreateDXGIFactory2(0x%X, %s) (0x%08X)", flags, IidName(riid), hr);
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
        Log(L"    IDXGISwapChain3::CheckColorSpaceSupport: %s",
            Describe(checkColorSpace));

#if HW2HDRPATCH_LOGGING
        // These only forward and log.
        const PatchOutcome setColorSpace =
            PatchSlot(sc3, kSwapChainSetColorSpace1, &HookSetColorSpace1,
                      reinterpret_cast<void**>(&g_origSetColorSpace1));
        Log(L"    IDXGISwapChain3::SetColorSpace1: %s", Describe(setColorSpace));

        const PatchOutcome present =
            PatchSlot(sc3, kSwapChainPresent, &HookPresent,
                      reinterpret_cast<void**>(&g_origPresent));
        Log(L"    IDXGISwapChain::Present: %s", Describe(present));

        const PatchOutcome resizeBuffers =
            PatchSlot(sc3, kSwapChainResizeBuffers, &HookResizeBuffers,
                      reinterpret_cast<void**>(&g_origResizeBuffers));
        Log(L"    IDXGISwapChain::ResizeBuffers: %s", Describe(resizeBuffers));

        const PatchOutcome getContainingOutput =
            PatchSlot(sc3, kSwapChainGetContainingOutput, &HookGetContainingOutput,
                      reinterpret_cast<void**>(&g_origGetContainingOutput));
        Log(L"    IDXGISwapChain::GetContainingOutput: %s",
            Describe(getContainingOutput));
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

    if (!g_outputPatched)
    {
        Log(L"[!] no IDXGIOutput6 to patch; the gate will do nothing");
    }
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out)
{
    const HRESULT hr = g_origCreateSwapChain(self, device, desc, out);
    Log(L"CreateSwapChain (0x%08X)", hr);
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
    LogSwapChainDesc(L"CreateSwapChainForHwnd", hr, desc);
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
    LogSwapChainDesc(L"CreateSwapChainForCoreWindow", hr, desc);
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
    LogSwapChainDesc(L"CreateSwapChainForComposition", hr, desc);
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
    if (moved)
    {
        Log(L"==== gate OPEN ====");
    }
    return moved;
}

bool CloseGate()
{
    const bool moved = g_gateOpen.exchange(false);
    if (moved)
    {
        Log(L"==== gate SHUT ====");
    }
    return moved;
}

bool InstallDxgiHooks()
{
    // Pinned, not loaded: resident without a load of our own.
    HMODULE dxgi = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &dxgi)
        || dxgi == nullptr)
    {
        Log(L"[!] dxgi.dll not loaded");
        return false;
    }

    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));

    if (g_origCreateFactory1 == nullptr || g_origCreateFactory2 == nullptr)
    {
        Log(L"[!] dxgi.dll exports no CreateDXGIFactory1/2");
        return false;
    }

    // Measured: the only two the host imports.
    const ImportPatch patches[] = {
        {"CreateDXGIFactory1", &HookCreateDXGIFactory1},
        {"CreateDXGIFactory2", &HookCreateDXGIFactory2},
    };

    if (PatchImports(L"dxgi.dll", patches, _countof(patches)) == 0)
    {
        Log(L"[!] nothing imports CreateDXGIFactory1/2 by name");
        return false;
    }
    return true;
}

}  // namespace hw2hdr
