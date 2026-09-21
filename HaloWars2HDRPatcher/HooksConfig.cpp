// HooksConfig.cpp - the configuration handed to the hook DLL.
//
// One flat '|'-separated key=value string, deliberately not a struct: the EXE
// and DLL then share no layout, only the key names, and neither has to include
// a header from the other. The same text serves both delivery routes - written
// into the game for the installer argument, or set as HW2HDRPATCH.
//
// '|' rather than a comma because GateFilter holds path substrings and '|'
// cannot legally appear in a Windows path. The DLL hard-codes the same one
// character; that duplication is the price of sharing no header.

#include "Launcher.h"

namespace launcher
{

const wchar_t* HookModeName(HookMode mode)
{
    return mode == HookMode::Sdr ? L"SDR" : L"HDR";
}

std::wstring BuildHooksArgs(const HooksConfig& config)
{
    std::wstring args;

    auto append = [&args](const wchar_t* key, const std::wstring& value) {
        if (!args.empty()) args += L'|';
        args += key;
        args += L'=';
        args += value;
    };

    append(L"Mode", HookModeName(config.mode));
    append(L"GateFilter", config.gateFilter);
    append(L"Log", config.log ? L"1" : L"0");

    // Mirrors what the DLL enforces anyway, so the two cannot disagree.
    append(L"LogToFile", (config.log && config.logToFile) ? L"1" : L"0");

    return args;
}

namespace
{

constexpr const wchar_t* kEnvKey = L"Environment";
constexpr const wchar_t* kEnvName = L"HW2HDRPATCH";

}  // namespace

bool HooksEnvironmentIsSet()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
    {
        return false;
    }

    const LSTATUS status =
        RegQueryValueExW(key, kEnvName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool SetHooksEnvironment(const std::wstring& value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_SET_VALUE, &key)
        != ERROR_SUCCESS)
    {
        return false;
    }

    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status =
        RegSetValueExW(key, kEnvName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool ClearHooksEnvironment()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnvKey, 0, KEY_SET_VALUE, &key)
        != ERROR_SUCCESS)
    {
        return false;
    }

    const LSTATUS status = RegDeleteValueW(key, kEnvName);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

}  // namespace launcher
