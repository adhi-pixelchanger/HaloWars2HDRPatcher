// Security.cpp - AppContainer file permissions.
//
// Without read/execute for S-1-15-2-1 (ALL APPLICATION PACKAGES), LoadLibraryW
// inside the game fails with ERROR_ACCESS_DENIED, which reaches the launcher as
// a remote thread exit code of 0 and nothing else - hence the check as well as
// the grant. Granted on the directory, inheritable, so it is needed once per
// deploy folder rather than once per file.

#include "Launcher.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace launcher {
namespace {

struct SidHolder {
    PSID sid = nullptr;
    explicit SidHolder(const wchar_t* sddl) {
        if (!ConvertStringSidToSidW(sddl, &sid)) sid = nullptr;
    }
    ~SidHolder() {
        if (sid != nullptr) LocalFree(sid);
    }
    SidHolder(const SidHolder&) = delete;
    SidHolder& operator=(const SidHolder&) = delete;
};

constexpr const wchar_t* kAllApplicationPackages = L"S-1-15-2-1";
constexpr const wchar_t* kAllRestrictedAppPackages = L"S-1-15-2-2";

bool GrantForSid(const std::wstring& path, PSID sid, bool isDirectory) {
    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &existingDacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        Warn(L"  GetNamedSecurityInfo(%s) failed: %lu", path.c_str(), status);
        return false;
    }

    EXPLICIT_ACCESS_W access = {};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance =
        isDirectory ? (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE) : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWCH>(sid);

    PACL newDacl = nullptr;
    status = SetEntriesInAclW(1, &access, existingDacl, &newDacl);
    if (status != ERROR_SUCCESS) {
        LocalFree(descriptor);
        Warn(L"  SetEntriesInAcl(%s) failed: %lu", path.c_str(), status);
        return false;
    }

    status = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   newDacl, nullptr);

    if (newDacl != nullptr) LocalFree(newDacl);
    LocalFree(descriptor);

    if (status != ERROR_SUCCESS) {
        Warn(L"  SetNamedSecurityInfo(%s) failed: %lu", path.c_str(), status);
        return false;
    }
    return true;
}

}  // namespace

bool GrantAppContainerAccess(const std::wstring& path, bool isDirectory) {
    SidHolder anyPackage(kAllApplicationPackages);
    SidHolder restrictedPackage(kAllRestrictedAppPackages);

    if (anyPackage.sid == nullptr) {
        Fail(L"  Could not build the ALL APPLICATION PACKAGES SID.");
        return false;
    }

    const bool ok = GrantForSid(path, anyPackage.sid, isDirectory);

    if (restrictedPackage.sid != nullptr) {
        GrantForSid(path, restrictedPackage.sid, isDirectory);
    }
    return ok;
}

// Requires read AND execute: an allow ACE with some other mask is not enough
// to map the DLL.
bool HasAppContainerReadAccess(const std::wstring& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &dacl, nullptr,
                              &descriptor) != ERROR_SUCCESS) {
        return false;
    }

    SidHolder anyPackage(kAllApplicationPackages);
    bool found = false;

    if (dacl != nullptr && anyPackage.sid != nullptr) {
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            LPVOID aceRaw = nullptr;
            if (!GetAce(dacl, i, &aceRaw)) continue;
            auto* header = static_cast<ACE_HEADER*>(aceRaw);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;

            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(aceRaw);
            if (!EqualSid(reinterpret_cast<PSID>(&ace->SidStart), anyPackage.sid)) {
                continue;
            }

            // Inherited ACEs carry mapped specific rights; freshly written
            // ones may still carry the generic bits.
            const DWORD specific = FILE_READ_DATA | FILE_EXECUTE;
            const bool hasSpecific = (ace->Mask & specific) == specific;
            const bool hasGeneric =
                (ace->Mask & (GENERIC_READ | GENERIC_EXECUTE))
                == (GENERIC_READ | GENERIC_EXECUTE);
            const bool hasAll = (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;

            if (hasSpecific || hasGeneric || hasAll) {
                found = true;
                break;
            }
        }
    }

    LocalFree(descriptor);
    return found;
}

}  // namespace launcher
