// DxgiHooks.cpp - the DXGI descriptor lie, and the gate that scopes it.

#include "Hooks.h"

#include <atomic>
#include <new>
#include <type_traits>

namespace hw2hdr
{
namespace
{

using PfnCreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PfnCreateDXGIFactory g_origCreateFactory1 = nullptr;
PfnCreateDXGIFactory2 g_origCreateFactory2 = nullptr;

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

#if HW2HDRPATCH_LOGGING

struct KnownIid
{
    const IID* iid;
    const wchar_t* name;
};

const KnownIid kKnownIids[] = {
    {&__uuidof(IUnknown), L"IUnknown"},
    {&__uuidof(IDXGIObject), L"IDXGIObject"},
    {&__uuidof(IDXGIDeviceSubObject), L"IDXGIDeviceSubObject"},
    {&__uuidof(IDXGIFactory), L"IDXGIFactory"},
    {&__uuidof(IDXGIFactory1), L"IDXGIFactory1"},
    {&__uuidof(IDXGIFactory2), L"IDXGIFactory2"},
    {&__uuidof(IDXGIFactory3), L"IDXGIFactory3"},
    {&__uuidof(IDXGIFactory4), L"IDXGIFactory4"},
    {&__uuidof(IDXGIFactory5), L"IDXGIFactory5"},
    {&__uuidof(IDXGIFactory6), L"IDXGIFactory6"},
    {&__uuidof(IDXGIFactory7), L"IDXGIFactory7"},
    {&__uuidof(IDXGIAdapter), L"IDXGIAdapter"},
    {&__uuidof(IDXGIAdapter1), L"IDXGIAdapter1"},
    {&__uuidof(IDXGIAdapter2), L"IDXGIAdapter2"},
    {&__uuidof(IDXGIAdapter3), L"IDXGIAdapter3"},
    {&__uuidof(IDXGIAdapter4), L"IDXGIAdapter4"},
    {&__uuidof(IDXGIOutput), L"IDXGIOutput"},
    {&__uuidof(IDXGIOutput1), L"IDXGIOutput1"},
    {&__uuidof(IDXGIOutput2), L"IDXGIOutput2"},
    {&__uuidof(IDXGIOutput3), L"IDXGIOutput3"},
    {&__uuidof(IDXGIOutput4), L"IDXGIOutput4"},
    {&__uuidof(IDXGIOutput5), L"IDXGIOutput5"},
    {&__uuidof(IDXGIOutput6), L"IDXGIOutput6"},
    {&__uuidof(IDXGISwapChain), L"IDXGISwapChain"},
    {&__uuidof(IDXGISwapChain1), L"IDXGISwapChain1"},
    {&__uuidof(IDXGISwapChain2), L"IDXGISwapChain2"},
    {&__uuidof(IDXGISwapChain3), L"IDXGISwapChain3"},
    {&__uuidof(IDXGISwapChain4), L"IDXGISwapChain4"},
    {&__uuidof(IDXGIDevice), L"IDXGIDevice"},
    {&__uuidof(IDXGIDevice1), L"IDXGIDevice1"},
    {&__uuidof(IDXGIDevice2), L"IDXGIDevice2"},
    {&__uuidof(IDXGIDevice3), L"IDXGIDevice3"},
    {&__uuidof(IDXGIDevice4), L"IDXGIDevice4"},
    {&__uuidof(IDXGISurface), L"IDXGISurface"},
    {&__uuidof(IDXGIResource), L"IDXGIResource"},
};

int IidIndex(REFIID riid)
{
    for (int index = 0; index < static_cast<int>(_countof(kKnownIids)); ++index)
    {
        if (riid == *kKnownIids[index].iid)
        {
            return index;
        }
    }
    return -1;
}

const wchar_t* IidName(REFIID riid)
{
    const int index = IidIndex(riid);
    return index >= 0 ? kKnownIids[index].name : L"(unrecognised)";
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

void LogQueryInterface(const wchar_t* who, REFIID riid, bool proxied)
{
    const int index = IidIndex(riid);
    if (index >= 0)
    {
        Log(proxied ? L"%s::QueryInterface(%s) -> ours"
                    : L"[!] %s::QueryInterface(%s) -> the real object, we lose it "
                      L"from here",
            who, kKnownIids[index].name);
        return;
    }

    Log(L"[!] %s::QueryInterface({%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X})"
        L" -> not ours to answer, the host now holds the real object",
        who, riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1],
        riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6],
        riid.Data4[7]);
}

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

#else  // !HW2HDRPATCH_LOGGING

#define LogQueryInterface(...) ((void)0)
#define LogSwapChainDesc(...) ((void)0)

#endif  // HW2HDRPATCH_LOGGING

#define FORWARD_LOG(returnType, method, parameters, arguments, LogArgs) \
    returnType STDMETHODCALLTYPE method parameters override \
    {                                                       \
        returnType retVal = m_real->method arguments;       \
        [[maybe_unused]]const long long result = (long long)retVal;         \
        Log LogArgs;                                        \
        return retVal;                                      \
    }
#define FORWARDV(method, parameters, arguments)             \
    void STDMETHODCALLTYPE method parameters override       \
    {                                                       \
        m_real->method arguments;                           \
        Log(L#method);                                      \
    }
#define FORWARD(returnType, method, parameters, arguments)  \
    FORWARD_LOG(returnType, method, parameters, arguments, (L#method L" (0x%08X)", (HRESULT)(result)))

template <typename Interface>
class Proxy : public Interface
{
public:
    explicit Proxy(Interface* real) : m_real(real)
    {
        m_real->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
        {
            return E_POINTER;
        }

        if (IsOurs(riid))
        {
            LogQueryInterface(Name(), riid, true);
            AddRef();
            *out = static_cast<Interface*>(this);
            return S_OK;
        }

        LogQueryInterface(Name(), riid, false);
        return m_real->QueryInterface(riid, out);
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_refs;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LogFlush();
        const ULONG refs = --m_refs;
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }

    FORWARD(HRESULT, SetPrivateData, (REFGUID name, UINT size, const void* data),
            (name, size, data))
    FORWARD(HRESULT, SetPrivateDataInterface,
            (REFGUID name, const IUnknown* unknown), (name, unknown))
    FORWARD(HRESULT, GetPrivateData, (REFGUID name, UINT* size, void* data),
            (name, size, data))
    FORWARD(HRESULT, GetParent, (REFIID riid, void** parent), (riid, parent))

protected:
    virtual ~Proxy()
    {
        m_real->Release();
    }

    virtual bool IsOurs(REFIID riid) const = 0;

#if HW2HDRPATCH_LOGGING
    virtual const wchar_t* Name() const = 0;
#endif

    Interface* const m_real;

private:
    std::atomic<ULONG> m_refs{1};
};

template <typename ProxyType, typename Interface>
bool Wrap(REFIID riid, void** slot)
{
    if (slot == nullptr || *slot == nullptr)
    {
        return false;
    }

    IUnknown* original = static_cast<IUnknown*>(*slot);
    Interface* real = nullptr;
    if (FAILED(original->QueryInterface(IID_PPV_ARGS(&real))) || real == nullptr)
    {
        return false;
    }

    auto* proxy = new (std::nothrow) ProxyType(real);
    real->Release();
    if (proxy == nullptr)
    {
        return false;
    }

    void* replacement = nullptr;
    const bool wrapped =
        SUCCEEDED(proxy->QueryInterface(riid, &replacement)) && replacement != nullptr;
    if (wrapped)
    {
        original->Release();
        *slot = replacement;
    }
    proxy->Release();
    return wrapped;
}

template <typename ProxyType, typename Interface, typename Slot>
bool Wrap(Slot** slot)
{
    return Wrap<ProxyType, Interface>(__uuidof(Slot),
                                      reinterpret_cast<void**>(slot));
}

// --- the output ------------------------------------------------------------

class OutputProxy final : public Proxy<IDXGIOutput6>
{
public:
    explicit OutputProxy(IDXGIOutput6* real) : Proxy(real) {}

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* desc) override
    {
        const HRESULT hr = m_real->GetDesc1(desc);

        const bool gated = IsGateOpen();
        Log(L"OutputProxy::GetDesc1 (0x%08X) [gate %s]", hr,
            gated ? L"open" : L"shut");

        if (FAILED(hr) || desc == nullptr || !gated)
        {
            return hr;
        }

        desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        desc->BitsPerColor = 8;

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

    FORWARD(HRESULT, GetDesc, (DXGI_OUTPUT_DESC* desc), (desc))

    FORWARD(HRESULT, GetDisplayModeList,
            (DXGI_FORMAT format, UINT flags, UINT* modeCount, DXGI_MODE_DESC* desc),
            (format, flags, modeCount, desc))
    FORWARD(HRESULT, FindClosestMatchingMode,
            (const DXGI_MODE_DESC* toMatch, DXGI_MODE_DESC* closest,
             IUnknown* device),
            (toMatch, closest, device))
    FORWARD(HRESULT, WaitForVBlank, (), ())
    FORWARD(HRESULT, TakeOwnership, (IUnknown* device, BOOL exclusive),
            (device, exclusive))
    FORWARDV(ReleaseOwnership, (), ())
    FORWARD(HRESULT, GetGammaControlCapabilities,
            (DXGI_GAMMA_CONTROL_CAPABILITIES* caps), (caps))
    FORWARD(HRESULT, SetGammaControl, (const DXGI_GAMMA_CONTROL* array), (array))
    FORWARD(HRESULT, GetGammaControl, (DXGI_GAMMA_CONTROL* array), (array))
    FORWARD(HRESULT, SetDisplaySurface, (IDXGISurface* surface), (surface))
    FORWARD(HRESULT, GetDisplaySurfaceData, (IDXGISurface* destination),
            (destination))
    FORWARD(HRESULT, GetFrameStatistics, (DXGI_FRAME_STATISTICS* stats), (stats))
    FORWARD(HRESULT, GetDisplayModeList1,
            (DXGI_FORMAT format, UINT flags, UINT* modeCount, DXGI_MODE_DESC1* desc),
            (format, flags, modeCount, desc))
    FORWARD(HRESULT, FindClosestMatchingMode1,
            (const DXGI_MODE_DESC1* toMatch, DXGI_MODE_DESC1* closest,
             IUnknown* device),
            (toMatch, closest, device))
    FORWARD(HRESULT, GetDisplaySurfaceData1, (IDXGIResource* destination),
            (destination))
    FORWARD(HRESULT, DuplicateOutput,
            (IUnknown* device, IDXGIOutputDuplication** duplication),
            (device, duplication))
    FORWARD(BOOL, SupportsOverlays, (), ())
    FORWARD(HRESULT, CheckOverlaySupport,
            (DXGI_FORMAT format, IUnknown* device, UINT* flags),
            (format, device, flags))

    FORWARD_LOG(HRESULT, CheckOverlayColorSpaceSupport,
                (DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colorSpace,
                 IUnknown* device, UINT* flags),
                (format, colorSpace, device, flags),
                (L"OutputProxy::CheckOverlayColorSpaceSupport(%s, colour space %d) (0x%08X)",
                 FormatName(format), static_cast<int>(colorSpace), result))

    FORWARD(HRESULT, DuplicateOutput1,
            (IUnknown* device, UINT flags, UINT formatCount,
             const DXGI_FORMAT* formats, IDXGIOutputDuplication** duplication),
            (device, flags, formatCount, formats, duplication))
    FORWARD(HRESULT, CheckHardwareCompositionSupport, (UINT* flags), (flags))

private:
#if HW2HDRPATCH_LOGGING
    const wchar_t* Name() const override
    {
        return L"OutputProxy";
    }
#endif

    bool IsOurs(REFIID riid) const override
    {
        return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject)
            || riid == __uuidof(IDXGIOutput) || riid == __uuidof(IDXGIOutput1)
            || riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3)
            || riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5)
            || riid == __uuidof(IDXGIOutput6);
    }
};

void WrapOutput(IDXGIOutput** output)
{
    if (output == nullptr || *output == nullptr)
    {
        return;
    }

    [[maybe_unused]] const bool wrapped = Wrap<OutputProxy, IDXGIOutput6>(output);
    Log(L"IDXGIOutput: wrapped=%d", wrapped );
}

// --- the swap chain --------------------------------------------------------

class SwapChainProxy final : public Proxy<IDXGISwapChain4>
{
public:
    explicit SwapChainProxy(IDXGISwapChain4* real) : Proxy(real) {}

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colorSpace,
                                                     UINT* support) override
    {
        const HRESULT hr = m_real->CheckColorSpaceSupport(colorSpace, support);

        const bool gated = IsGateOpen();
        Log(L"SwapChainProxy::CheckColorSpaceSupport(%d %s) [gate %s]",
            static_cast<int>(colorSpace), IsHdr(colorSpace) ? L"HDR" : L"SDR",
            gated ? L"open" : L"shut");

        if (SUCCEEDED(hr) && support != nullptr && gated && IsHdr(colorSpace))
        {
            *support = 0;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override
    {
        const HRESULT hr = m_real->GetContainingOutput(output);
        Log(L"SwapChainProxy::GetContainingOutput (0x%08X)", hr);
        if (SUCCEEDED(hr))
        {
            WrapOutput(output);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override
    {
        const HRESULT result = m_real->GetDesc1(desc);
        LogSwapChainDesc(L"SwapChainProxy::GetDesc1", result, desc);
        return result;
    }

    FORWARD_LOG(HRESULT, GetRestrictToOutput, (IDXGIOutput** output), (output),
                (L"[!] SwapChainProxy::GetRestrictToOutput (0x%08X) - unwrapped",
                 result))
    FORWARD_LOG(HRESULT, SetFullscreenState,
                (BOOL fullscreen, IDXGIOutput* target), (fullscreen, target),
                (L"SwapChainProxy::SetFullscreenState(%d, target %s) (0x%08X)", fullscreen,
                 target != nullptr ? L"given" : L"null", result))
    FORWARD_LOG(HRESULT, GetFullscreenState,
                (BOOL* fullscreen, IDXGIOutput** target), (fullscreen, target),
                (L"[!] SwapChainProxy::GetFullscreenState (0x%08X) - unwrapped",
                 result))
    FORWARD_LOG(HRESULT, GetDevice, (REFIID riid, void** device), (riid, device),
                (L"SwapChainProxy::GetDevice(%s) (0x%08X)", IidName(riid), result))
    FORWARD_LOG(HRESULT, Present, (UINT syncInterval, UINT flags),
                (syncInterval, flags),
                (L"SwapChainProxy::Present(sync %u, flags 0x%X) (0x%08X)", syncInterval,
                 flags, result))
    FORWARD_LOG(HRESULT, GetBuffer, (UINT buffer, REFIID riid, void** surface),
                (buffer, riid, surface),
                (L"SwapChainProxy::GetBuffer(%u, %s) (0x%08X)", buffer, IidName(riid), result))
    FORWARD(HRESULT, GetDesc, (DXGI_SWAP_CHAIN_DESC* desc), (desc))

    FORWARD_LOG(HRESULT, ResizeBuffers,
                (UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat,
                 UINT swapChainFlags),
                (bufferCount, width, height, newFormat, swapChainFlags),
                (L"host ResizeBuffers(%u buffers, %ux%u, %s, flags 0x%X) (0x%08X)",
                 bufferCount, width, height, FormatName(newFormat), swapChainFlags, result))

    FORWARD_LOG(HRESULT, SetColorSpace1, (DXGI_COLOR_SPACE_TYPE colorSpace),
                (colorSpace),
                (L"host SetColorSpace1(%d %s) (0x%08X)", static_cast<int>(colorSpace),
                 IsHdr(colorSpace) ? L"HDR" : L"SDR", result))
    FORWARD(HRESULT, ResizeTarget, (const DXGI_MODE_DESC* parameters), (parameters))
    FORWARD(HRESULT, GetFrameStatistics, (DXGI_FRAME_STATISTICS* stats), (stats))
    FORWARD(HRESULT, GetLastPresentCount, (UINT* lastPresentCount),
            (lastPresentCount))
    FORWARD(HRESULT, GetFullscreenDesc, (DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc),
            (desc))
    FORWARD(HRESULT, GetHwnd, (HWND* hwnd), (hwnd))
    FORWARD(HRESULT, GetCoreWindow, (REFIID riid, void** unknown), (riid, unknown))

    FORWARD_LOG(HRESULT, Present1,
                (UINT syncInterval, UINT presentFlags,
                 const DXGI_PRESENT_PARAMETERS* parameters),
                (syncInterval, presentFlags, parameters),
                (L"SwapChainProxy::Present1(sync %u, flags 0x%X) (0x%08X)", syncInterval,
                 presentFlags, result))

    FORWARD(BOOL, IsTemporaryMonoSupported, (), ())
    FORWARD(HRESULT, SetBackgroundColor, (const DXGI_RGBA* color), (color))
    FORWARD(HRESULT, GetBackgroundColor, (DXGI_RGBA* color), (color))
    FORWARD(HRESULT, SetRotation, (DXGI_MODE_ROTATION rotation), (rotation))
    FORWARD(HRESULT, GetRotation, (DXGI_MODE_ROTATION* rotation), (rotation))
    FORWARD(HRESULT, SetSourceSize, (UINT width, UINT height), (width, height))
    FORWARD(HRESULT, GetSourceSize, (UINT* width, UINT* height), (width, height))
    FORWARD(HRESULT, SetMaximumFrameLatency, (UINT maxLatency), (maxLatency))
    FORWARD(HRESULT, GetMaximumFrameLatency, (UINT* maxLatency), (maxLatency))
    FORWARD_LOG(HANDLE, GetFrameLatencyWaitableObject, (), (),
                (L"SwapChainProxy::GetFrameLatencyWaitableObject -> %p", result))
    FORWARD(HRESULT, SetMatrixTransform, (const DXGI_MATRIX_3X2_F* matrix), (matrix))
    FORWARD(HRESULT, GetMatrixTransform, (DXGI_MATRIX_3X2_F* matrix), (matrix))

    FORWARD(UINT, GetCurrentBackBufferIndex, (), ())
    FORWARD_LOG(HRESULT, ResizeBuffers1,
                (UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format,
                 UINT swapChainFlags, const UINT* creationNodeMask,
                 IUnknown* const* presentQueue),
                (bufferCount, width, height, format, swapChainFlags,
                 creationNodeMask, presentQueue),
                (L"host ResizeBuffers1(%u buffers, %ux%u, %s, flags 0x%X) (0x%08X)",
                 bufferCount, width, height, FormatName(format), swapChainFlags, result))
    FORWARD_LOG(HRESULT, SetHDRMetaData,
                (DXGI_HDR_METADATA_TYPE type, UINT size, void* metaData),
                (type, size, metaData),
                (L"host SetHDRMetaData(type %d, %u bytes) (0x%08X)", static_cast<int>(type),
                 size, result))

private:
#if HW2HDRPATCH_LOGGING
    const wchar_t* Name() const override
    {
        return L"SwapChainProxy";
    }
#endif

    bool IsOurs(REFIID riid) const override
    {
        return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject)
            || riid == __uuidof(IDXGIDeviceSubObject)
            || riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1)
            || riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3)
            || riid == __uuidof(IDXGISwapChain4);
    }
};

template <typename Slot>
void WrapSwapChain(Slot** swapChain)
{
    if (swapChain == nullptr || *swapChain == nullptr)
    {
        return;
    }

    [[maybe_unused]] const bool wrapped =
        Wrap<SwapChainProxy, IDXGISwapChain4>(swapChain);
    Log(L"IDXGISwapChain: %s",
        wrapped ? L"wrapped" : L"[!] unwrapped, potentially fatal");
}

// --- the factory -----------------------------------------------------------

class FactoryProxy final : public Proxy<IDXGIFactory7>
{
public:
    explicit FactoryProxy(IDXGIFactory7* real) : Proxy(real) {}

    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
        IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc,
        IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override
    {
        const HRESULT hr = m_real->CreateSwapChainForCoreWindow(
            device, window, desc, restrictToOutput, swapChain);
        LogSwapChainDesc(L"host CreateSwapChainForCoreWindow", hr, desc);
        if (SUCCEEDED(hr))
        {
            WrapSwapChain(swapChain);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
        IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
        IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override
    {
        const HRESULT hr = m_real->CreateSwapChainForHwnd(
            device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);
        LogSwapChainDesc(L"[!] host CreateSwapChainForHwnd, unwrapped", hr, desc);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
        IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
        IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) override
    {
        const HRESULT hr = m_real->CreateSwapChainForComposition(
            device, desc, restrictToOutput, swapChain);
        LogSwapChainDesc(L"[!] host CreateSwapChainForComposition, unwrapped", hr,
                         desc);
        return hr;
    }

    FORWARD_LOG(HRESULT, EnumAdapters, (UINT index, IDXGIAdapter** adapter),
                (index, adapter),
                (L"FactoryProxy::EnumAdapters(%u) (0x%08X)", index, result))
    FORWARD_LOG(HRESULT, EnumAdapters1, (UINT index, IDXGIAdapter1** adapter),
                (index, adapter),
                (L"FactoryProxy::EnumAdapters1(%u) (0x%08X)", index, result))
    FORWARD_LOG(HRESULT, EnumAdapterByLuid, (LUID luid, REFIID riid, void** adapter),
                (luid, riid, adapter),
                (L"FactoryProxy::EnumAdapterByLuid(%s) (0x%08X)", IidName(riid),
                 result))
    FORWARD_LOG(HRESULT, EnumWarpAdapter, (REFIID riid, void** adapter),
                (riid, adapter),
                (L"FactoryProxy::EnumWarpAdapter(%s) (0x%08X)", IidName(riid),
                 result))
    FORWARD_LOG(HRESULT, EnumAdapterByGpuPreference,
                (UINT index, DXGI_GPU_PREFERENCE preference, REFIID riid,
                 void** adapter),
                (index, preference, riid, adapter),
                (L"FactoryProxy::EnumAdapterByGpuPreference(%u, preference %d, %s) "
                 L"(0x%08X)",
                 index, static_cast<int>(preference), IidName(riid), result))

    FORWARD(HRESULT, CreateSoftwareAdapter, (HMODULE module, IDXGIAdapter** adapter),
            (module, adapter))

    FORWARD_LOG(HRESULT, CreateSwapChain,
                (IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                 IDXGISwapChain** swapChain),
                (device, desc, swapChain),
                (L"[!] host CreateSwapChain (0x%08X): %s, %u buffers, flags 0x%X",
                 result,
                 desc ? FormatName(desc->BufferDesc.Format)
                                 : L"(no descriptor)",
                 desc ? desc->BufferCount : 0,
                 desc ? desc->Flags : 0))

    FORWARD_LOG(HRESULT, MakeWindowAssociation, (HWND window, UINT flags),
                (window, flags),
                (L"FactoryProxy::MakeWindowAssociation(flags 0x%X) (0x%08X)", flags, result))

    FORWARD(HRESULT, GetWindowAssociation, (HWND* window), (window))
    FORWARD(BOOL, IsCurrent, (), ())
    FORWARD(BOOL, IsWindowedStereoEnabled, (), ())
    FORWARD(HRESULT, GetSharedResourceAdapterLuid, (HANDLE resource, LUID* luid),
            (resource, luid))
    FORWARD(HRESULT, RegisterStereoStatusWindow,
            (HWND window, UINT message, DWORD* cookie), (window, message, cookie))
    FORWARD(HRESULT, RegisterStereoStatusEvent, (HANDLE event, DWORD* cookie),
            (event, cookie))
    FORWARDV(UnregisterStereoStatus, (DWORD cookie), (cookie))
    FORWARD(HRESULT, RegisterOcclusionStatusWindow,
            (HWND window, UINT message, DWORD* cookie), (window, message, cookie))
    FORWARD(HRESULT, RegisterOcclusionStatusEvent, (HANDLE event, DWORD* cookie),
            (event, cookie))
    FORWARDV(UnregisterOcclusionStatus, (DWORD cookie), (cookie))
    FORWARD(UINT, GetCreationFlags, (), ())
    FORWARD(HRESULT, CheckFeatureSupport,
            (DXGI_FEATURE feature, void* data, UINT dataSize),
            (feature, data, dataSize))
    FORWARD(HRESULT, RegisterAdaptersChangedEvent, (HANDLE event, DWORD* cookie),
            (event, cookie))
    FORWARD(HRESULT, UnregisterAdaptersChangedEvent, (DWORD cookie), (cookie))

private:
#if HW2HDRPATCH_LOGGING
    const wchar_t* Name() const override
    {
        return L"FactoryProxy";
    }
#endif

    bool IsOurs(REFIID riid) const override
    {
        return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject)
            || riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1)
            || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3)
            || riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5)
            || riid == __uuidof(IDXGIFactory6) || riid == __uuidof(IDXGIFactory7);
    }
};

#undef FORWARD
#undef FORWARDV
#undef FORWARD_LOG

void AdoptHostFactory(REFIID riid, void** out)
{
    if (out == nullptr || *out == nullptr)
    {
        return;
    }
    [[maybe_unused]] const bool wrapped = Wrap<FactoryProxy, IDXGIFactory7>(riid, out);
    Log(L"Host created a DXGI factory, asking for %s: wrapped=%d", IidName(riid), wrapped);
}

HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory1(riid, out);
    Log(L"host called CreateDXGIFactory1 (0x%08X)", hr);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(riid, out);
    }
    return hr;
}

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    const HRESULT hr = g_origCreateFactory2(flags, riid, out);
    Log(L"host called CreateDXGIFactory2(flags 0x%X) (0x%08X)", flags, hr);
    if (SUCCEEDED(hr) && out != nullptr)
    {
        AdoptHostFactory(riid, out);
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
    HMODULE dxgi = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &dxgi)
        || dxgi == nullptr)
    {
        Log(L"[!] dxgi.dll is not loaded in the host - nothing to hook.");
        return false;
    }

    g_origCreateFactory1 = reinterpret_cast<PfnCreateDXGIFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    g_origCreateFactory2 = reinterpret_cast<PfnCreateDXGIFactory2>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));

    if (g_origCreateFactory1 == nullptr || g_origCreateFactory2 == nullptr)
    {
        Log(L"[!] dxgi.dll did not export the expected factory entry points.");
        return false;
    }

    const ImportPatch patches[] = {
        {"CreateDXGIFactory1", &HookCreateDXGIFactory1},
        {"CreateDXGIFactory2", &HookCreateDXGIFactory2},
    };

    const size_t total = PatchImports(L"dxgi.dll", patches, _countof(patches));
    if (total == 0)
    {
        Log(L"[!] Nothing imports CreateDXGIFactory* from dxgi.dll by name.");
        return false;
    }

    Log(L"Waiting for the host to create its DXGI factory (%zu import slot(s) "
        L"redirected).", total);
    return true;
}

}  // namespace hw2hdr
