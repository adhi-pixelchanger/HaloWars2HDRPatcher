// AppModel.cpp - finding the installed package and its AUMID.

#include "Launcher.h"

#include <appmodel.h>
#include <algorithm>
#include <cwctype>

namespace launcher {
namespace {

constexpr const wchar_t* kLaunchContractKey =
    L"Software\\Classes\\Extensions\\ContractId\\Windows.Launch\\PackageId";

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::vector<std::wstring> EnumerateSubKeys(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return names;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD length = _countof(name);
        const LONG status =
            RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) break;
        names.emplace_back(name, length);
    }
    RegCloseKey(key);
    return names;
}

std::wstring FirstPraid(const std::wstring& packageFullName) {
    const std::wstring path =
        std::wstring(kLaunchContractKey) + L"\\" + packageFullName + L"\\ActivatableClassId";
    const auto praids = EnumerateSubKeys(HKEY_CURRENT_USER, path);
    return praids.empty() ? std::wstring() : praids.front();
}

}  // namespace

std::vector<std::wstring> EnumerateLaunchablePackages() {
    return EnumerateSubKeys(HKEY_CURRENT_USER, kLaunchContractKey);
}

std::wstring FindPackageFullName(const std::wstring& needle) {
    const std::wstring lowerNeedle = ToLower(needle);
    std::vector<std::wstring> matches;

    for (const auto& fullName : EnumerateLaunchablePackages()) {
        if (ToLower(fullName).find(lowerNeedle) != std::wstring::npos) {
            matches.push_back(fullName);
        }
    }

    if (matches.empty()) return L"";

    if (matches.size() > 1) {
        Warn(L"%zu packages matched \"%s\":", matches.size(), needle.c_str());
        for (const auto& m : matches) Warn(L"    %s", m.c_str());
        Warn(L"Using the first one. Narrow the package filter under Advanced to "
             L"be specific.");
    }
    return matches.front();
}

std::wstring PackageFamilyFromFullName(const std::wstring& fullName) {
    UINT32 length = 0;
    LONG status = PackageFamilyNameFromFullName(fullName.c_str(), &length, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER || length == 0) return L"";

    std::wstring family(length, L'\0');
    status = PackageFamilyNameFromFullName(fullName.c_str(), &length, family.data());
    if (status != ERROR_SUCCESS) return L"";

    family.resize(length == 0 ? 0 : length - 1);
    return family;
}

std::wstring GetFirstAumid(const std::wstring& packageFullName) {
    PACKAGE_INFO_REFERENCE infoRef = nullptr;
    if (OpenPackageInfoByFullName(packageFullName.c_str(), 0, &infoRef) ==
            ERROR_SUCCESS &&
        infoRef != nullptr) {
        UINT32 bufferLength = 0;
        UINT32 count = 0;
        LONG status = GetPackageApplicationIds(infoRef, &bufferLength, nullptr, &count);
        if (status == ERROR_INSUFFICIENT_BUFFER && bufferLength > 0) {
            std::vector<BYTE> buffer(bufferLength);
            status = GetPackageApplicationIds(infoRef, &bufferLength, buffer.data(), &count);
            if (status == ERROR_SUCCESS && count > 0) {
                auto* ids = reinterpret_cast<PWSTR*>(buffer.data());
                std::wstring aumid = ids[0];
                ClosePackageInfo(infoRef);
                if (!aumid.empty()) return aumid;
                infoRef = nullptr;
            }
        }
        if (infoRef != nullptr) ClosePackageInfo(infoRef);
    }

    const std::wstring family = PackageFamilyFromFullName(packageFullName);
    const std::wstring praid = FirstPraid(packageFullName);
    if (family.empty() || praid.empty()) return L"";
    return family + L"!" + praid;
}

std::wstring GetPackageInstallPath(const std::wstring& packageFullName) {
    UINT32 length = 0;
    LONG status = GetPackagePathByFullName(packageFullName.c_str(), &length, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER || length == 0) return L"";

    std::wstring path(length, L'\0');
    status = GetPackagePathByFullName(packageFullName.c_str(), &length, path.data());
    if (status != ERROR_SUCCESS) return L"";

    path.resize(length == 0 ? 0 : length - 1);
    return path;
}

}  // namespace launcher
