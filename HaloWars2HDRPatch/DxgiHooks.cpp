// DxgiHooks.cpp - the pair of DXGI lies, and the gate that scopes them.
//
// Invariant: with the gate shut every hook returns the original's result
// untouched, and no hook initiates a DXGI call of its own.
//
// The install creates no factory - it redirects the host's imports of
// CreateDXGIFactory/1/2 and patches vtables from the factory the host creates.
// Creating one here would run inside the launcher's suspend window, where it
// can block on a lock a frozen thread holds.

#include "Hooks.h"

#include <atomic>

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

// IDXGISwapChain3 36 GetCurrentBackBufferIndex, 37 CheckColorSpaceSupport,
// 38 SetColorSpace1.
constexpr size_t kSwapChainCheckColorSpaceSupport = 37;
constexpr size_t kSwapChainSetColorSpace1 = 38;

// CreateDXGIFactory and CreateDXGIFactory1 share a signature.
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
using PfnSetColorSpace1 =
    HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

PfnCreateDXGIFactory g_origCreateFactory = nullptr;
PfnCreateDXGIFactory g_origCreateFactory1 = nullptr;
PfnCreateDXGIFactory2 g_origCreateFactory2 = nullptr;

PfnGetDesc1 g_origGetDesc1 = nullptr;
PfnCreateSwapChain g_origCreateSwapChain = nullptr;
PfnForHwnd g_origForHwnd = nullptr;
PfnForCoreWindow g_origForCoreWindow = nullptr;
PfnForComposition g_origForComposition = nullptr;
PfnCheckColorSpaceSupport g_origCheckColorSpace = nullptr;
PfnSetColorSpace1 g_origSetColorSpace1 = nullptr;

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

// --- the three hooks -------------------------------------------------------

HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGIOutput6* self, DXGI_OUTPUT_DESC1* desc)
{
    const HRESULT hr = g_origGetDesc1(self, desc);
    if (FAILED(hr) || desc == nullptr || !IsGateOpen())
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
    if (SUCCEEDED(hr) && support != nullptr && IsGateOpen() && IsHdr(colorSpace))
    {
        *support = 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetColorSpace1(IDXGISwapChain3* self,
                                             DXGI_COLOR_SPACE_TYPE colorSpace)
{
    const bool gated = IsGateOpen();

    // Whether the host re-asserts its colour space after a close decides whether
    // HDR comes back, and this is the only place that is visible.
    Log(L"host SetColorSpace1(%d %s) [gate %s]", static_cast<int>(colorSpace),
        IsHdr(colorSpace) ? L"HDR" : L"SDR", gated ? L"open" : L"shut");

    if (gated && IsHdr(colorSpace))
    {
        const HRESULT hr =
            g_origSetColorSpace1(self, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        Log(L"    downgraded to sRGB (0x%08X)", hr);

        // The host likely does not handle an error here.
        return SUCCEEDED(hr) ? hr : S_OK;
    }

    return g_origSetColorSpace1(self, colorSpace);
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

HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory(riid, out);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(static_cast<IUnknown*>(*out));
    }
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory1(riid, out);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(static_cast<IUnknown*>(*out));
    }
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(static_cast<IUnknown*>(*out));
    }
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

        [[maybe_unused]] const PatchOutcome setColorSpace =
            PatchSlot(sc3, kSwapChainSetColorSpace1, &HookSetColorSpace1,
                      reinterpret_cast<void**>(&g_origSetColorSpace1));
        Log(L"    IDXGISwapChain3::SetColorSpace1 (slot %zu): %s",
            kSwapChainSetColorSpace1, Describe(setColorSpace));

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
    if (SUCCEEDED(hr) && out != nullptr)
    {
        HookSwapChain(*out);
    }
    return hr;
}

}  // namespace

bool OpenGate()
{
    return !g_gateOpen.exchange(true);
}

bool CloseGate()
{
    return g_gateOpen.exchange(false);
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

    g_origCreateFactory = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")));
    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));

    if (g_origCreateFactory == nullptr || g_origCreateFactory1 == nullptr
        || g_origCreateFactory2 == nullptr)
    {
        Log(L"[!] dxgi.dll did not export the expected factory entry points.");
        return false;
    }

    const ImportPatch patches[] = {
        {"CreateDXGIFactory", &HookCreateDXGIFactory},
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
