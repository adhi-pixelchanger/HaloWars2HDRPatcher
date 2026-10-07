// HooksConfig.cpp - the configuration handed to the hook DLL.

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
