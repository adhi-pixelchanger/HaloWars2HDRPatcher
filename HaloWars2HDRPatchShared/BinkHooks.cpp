#include "Hooks.h"

#include <algorithm>
#include <cwctype>

namespace hw2hdr
{
namespace
{

using PfnBinkOpen = void*(__stdcall*)(const char* name, UINT32 flags);
using PfnBinkClose = void(__stdcall*)(void* bink);

constexpr UINT32 kBinkFromMemory = 0x04000000;

PfnBinkOpen g_origOpen = nullptr;
PfnBinkClose g_origClose = nullptr;

bool MovieMatchesFilter(const char* name, const std::wstring& filter)
{
    if (filter.empty())
    {
        return true;
    }
    if (name == nullptr)
    {
        return false;
    }

    wchar_t wide[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);

    std::wstring haystack(wide);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    size_t start = 0;
    while (start <= filter.size())
    {
        size_t end = filter.find(L';', start);
        if (end == std::wstring::npos)
        {
            end = filter.size();
        }

        std::wstring token = filter.substr(start, end - start);
        std::transform(token.begin(), token.end(), token.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        while (!token.empty() && token.front() == L' ')
        {
            token.erase(0, 1);
        }
        while (!token.empty() && token.back() == L' ')
        {
            token.pop_back();
        }

        if (!token.empty() && haystack.find(token) != std::wstring::npos)
        {
            return true;
        }
        start = end + 1;
    }
    return false;
}

void* __stdcall HookBinkOpen(const char* name, UINT32 flags)
{
    const bool fromMemory = (flags & kBinkFromMemory) != 0;
    const bool gateThisMovie =
        !fromMemory && MovieMatchesFilter(name, g_config.gateFilter);

    if (fromMemory)
    {
        Log(L"BinkOpen(from memory, flags=0x%08X)", flags);
    }
    else
    {
        Log(L"BinkOpen(\"%S\", flags=0x%08X)%s", name != nullptr ? name : "(null)",
            flags, gateThisMovie ? L" gated" : L"");
    }

    if (gateThisMovie)
    {
        OpenGate();
    }
    else
    {
        CloseGate();
    }
    return g_origOpen(name, flags);
}

void __stdcall HookBinkClose(void* bink)
{
    Log(L"BinkClose");
    CloseGate();
    g_origClose(bink);
}

}  // namespace

bool ResolveBinkExports()
{
    if (g_origOpen != nullptr && g_origClose != nullptr)
    {
        return true;
    }

    HMODULE bink = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, kBinkModule,
                            &bink)
        || bink == nullptr)
    {
        return false;
    }

    auto open = reinterpret_cast<PfnBinkOpen>(GetProcAddress(bink, "BinkOpen"));
    auto close = reinterpret_cast<PfnBinkClose>(GetProcAddress(bink, "BinkClose"));
    if (open == nullptr || close == nullptr)
    {
        Log(L"[!] %s does not export BinkOpen/BinkClose", kBinkModule);
        return false;
    }

    g_origOpen = open;
    g_origClose = close;
    return true;
}

bool InstallBinkHooks()
{
    if (!ResolveBinkExports())
    {
        return false;
    }

    const ImportPatch patches[] = {
        {"BinkOpen", &HookBinkOpen},
        {"BinkClose", &HookBinkClose},
    };

    return PatchImports(kBinkModule, patches, _countof(patches)) > 0;
}

}  // namespace hw2hdr
