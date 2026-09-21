// Diagnostics.cpp - whether this process can be injected at all.
//
// Only MicrosoftSignedOnly (Code Integrity Guard) blocks this design.
// DisableExtensionPoints blocks SetWindowsHookEx loaders, not
// CreateRemoteThread; Arbitrary Code Guard does not apply, because no
// executable memory is allocated and the vtable patches touch .rdata.

#include "Launcher.h"

#include <psapi.h>
#include <wintrust.h>
#include <softpub.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wintrust.lib")

namespace launcher {

MitigationReport QueryMitigations(HANDLE process) {
    MitigationReport report;
    if (process == nullptr) return report;

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature = {};
    if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature,
                                   sizeof(signature))) {
        report.queried = true;
        report.microsoftSignedOnly = signature.MicrosoftSignedOnly != 0;
        report.storeSignedOnly = signature.StoreSignedOnly != 0;
        report.signatureMitigationOptIn = signature.MitigationOptIn != 0;
    }

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad = {};
    if (GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &imageLoad,
                                   sizeof(imageLoad))) {
        report.queried = true;
        report.noRemoteImages = imageLoad.NoRemoteImages != 0;
        report.noLowMandatoryLabelImages = imageLoad.NoLowMandatoryLabelImages != 0;
        report.preferSystem32Images = imageLoad.PreferSystem32Images != 0;
    }

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints = {};
    if (GetProcessMitigationPolicy(process, ProcessExtensionPointDisablePolicy,
                                   &extensionPoints, sizeof(extensionPoints))) {
        report.queried = true;
        report.disableExtensionPoints = extensionPoints.DisableExtensionPoints != 0;
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode = {};
    if (GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamicCode,
                                   sizeof(dynamicCode))) {
        report.queried = true;
        report.prohibitDynamicCode = dynamicCode.ProhibitDynamicCode != 0;
        report.allowThreadOptOut = dynamicCode.AllowThreadOptOut != 0;
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg = {};
    if (GetProcessMitigationPolicy(process, ProcessControlFlowGuardPolicy, &cfg,
                                   sizeof(cfg))) {
        report.queried = true;
        report.cfgEnabled = cfg.EnableControlFlowGuard != 0;
        report.cfgStrict = cfg.StrictMode != 0;
    }

    return report;
}

namespace {
const wchar_t* YesNo(bool value) { return value ? L"YES" : L"no"; }
}  // namespace

void ReportMitigations(const MitigationReport& report) {
    Rule(L"process mitigation policies");

    if (!report.queried) {
        Fail(L"GetProcessMitigationPolicy failed for every policy class (%lu).",
             GetLastError());
        return;
    }

    Info(L"  Signature policy");
    Info(L"    MicrosoftSignedOnly ............ %s   <- decides everything",
         YesNo(report.microsoftSignedOnly));
    Info(L"    StoreSignedOnly ................ %s", YesNo(report.storeSignedOnly));
    Info(L"    MitigationOptIn ................ %s",
         YesNo(report.signatureMitigationOptIn));
    Info(L"  Image load policy");
    Info(L"    NoRemoteImages ................. %s", YesNo(report.noRemoteImages));
    Info(L"    NoLowMandatoryLabelImages ...... %s   <- Mark of the Web",
         YesNo(report.noLowMandatoryLabelImages));
    Info(L"    PreferSystem32Images ........... %s", YesNo(report.preferSystem32Images));
    Info(L"  Extension point policy");
    Info(L"    DisableExtensionPoints ......... %s   (hook-based loaders only)",
         YesNo(report.disableExtensionPoints));
    Info(L"  Dynamic code policy (not relevant to this injector)");
    Info(L"    ProhibitDynamicCode (ACG) ...... %s", YesNo(report.prohibitDynamicCode));
    Info(L"    AllowThreadOptOut .............. %s", YesNo(report.allowThreadOptOut));
    Info(L"  Control flow guard");
    Info(L"    EnableControlFlowGuard ......... %s", YesNo(report.cfgEnabled));
    Info(L"    StrictMode ..................... %s", YesNo(report.cfgStrict));
}

std::wstring InjectionVerdict(const MitigationReport& report) {
    if (!report.queried) {
        return L"Could not read the process mitigation policies. No verdict.";
    }

    if (report.microsoftSignedOnly) {
        return L"BLOCKED: MicrosoftSignedOnly (Code Integrity Guard) is set on this "
               L"process. No unsigned or third-party-signed DLL can be mapped into "
               L"it by any technique. This is the only condition under which the "
               L"\"impossible to inject\" conclusion is actually correct.";
    }

    std::wstring verdict =
        L"INJECTABLE: MicrosoftSignedOnly is not set, so a remote LoadLibraryW is "
        L"permitted.";

    if (report.disableExtensionPoints) {
        verdict +=
            L"  DisableExtensionPoints IS set, which blocks CBT-hook style "
            L"injectors. That is a different mechanism from this one, not a "
            L"general block.";
    }
    if (report.noLowMandatoryLabelImages) {
        verdict +=
            L"  NoLowMandatoryLabelImages is set: the DLL must not carry a Mark of "
            L"the Web. If this payload arrived in a ZIP or a download, unblock it "
            L"from its Properties page.";
    }
    if (report.preferSystem32Images) {
        verdict += L"  PreferSystem32Images is set; give the payload a name that "
                   L"does not collide with a system DLL.";
    }
    return verdict;
}

// ---------------------------------------------------------------------------
// payload inspection
// ---------------------------------------------------------------------------

DllArch QueryDllArchitecture(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return DllArch::Unknown;

    DllArch arch = DllArch::Unknown;
    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;

    if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE) {
        if (SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) !=
            INVALID_SET_FILE_POINTER) {
            DWORD signature = 0;
            IMAGE_FILE_HEADER fileHeader = {};
            if (ReadFile(file, &signature, sizeof(signature), &read, nullptr) &&
                signature == IMAGE_NT_SIGNATURE &&
                ReadFile(file, &fileHeader, sizeof(fileHeader), &read, nullptr) &&
                read == sizeof(fileHeader)) {
                switch (fileHeader.Machine) {
                    case IMAGE_FILE_MACHINE_I386:  arch = DllArch::X86;   break;
                    case IMAGE_FILE_MACHINE_AMD64: arch = DllArch::X64;   break;
                    case IMAGE_FILE_MACHINE_ARM64: arch = DllArch::Arm64; break;
                    default: break;
                }
            }
        }
    }

    CloseHandle(file);
    return arch;
}

const wchar_t* DllArchName(DllArch arch) {
    switch (arch) {
        case DllArch::X86:   return L"x86 (32-bit)";
        case DllArch::X64:   return L"x64";
        case DllArch::Arm64: return L"ARM64";
        default:             return L"unknown";
    }
}

SignatureState QueryAuthenticode(const std::wstring& path, DWORD* status) {
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    WINTRUST_DATA data = {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;

    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),
                                       &action, &data);

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

    if (status != nullptr) *status = static_cast<DWORD>(result);

    if (result == ERROR_SUCCESS) return SignatureState::Trusted;
    if (static_cast<DWORD>(result) == TRUST_E_NOSIGNATURE) return SignatureState::NotSigned;
    if (static_cast<DWORD>(result) == TRUST_E_EXPLICIT_DISTRUST ||
        static_cast<DWORD>(result) == TRUST_E_SUBJECT_NOT_TRUSTED ||
        static_cast<DWORD>(result) == CERT_E_UNTRUSTEDROOT) {
        return SignatureState::Untrusted;
    }
    return SignatureState::Error;
}

const wchar_t* SignatureStateName(SignatureState state) {
    switch (state) {
        case SignatureState::Trusted:   return L"signed and trusted";
        case SignatureState::NotSigned: return L"unsigned";
        case SignatureState::Untrusted: return L"signed but not trusted";
        default:                        return L"could not be determined";
    }
}

void InspectPayload(const std::wstring& dllPath) {
    Rule(L"payload");
    Info(L"  Path ........... %s", dllPath.c_str());

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail(L"  File does not exist.");
        return;
    }

    const DllArch arch = QueryDllArchitecture(dllPath);
    Info(L"  Architecture ... %s%s", DllArchName(arch),
         (arch == DllArch::X64) ? L"" : L"   <- must be x64");

    DWORD trustStatus = 0;
    const SignatureState signature = QueryAuthenticode(dllPath, &trustStatus);
    Info(L"  Signature ...... %s (0x%08X)", SignatureStateName(signature), trustStatus);

    // Without this, a remote LoadLibraryW returning NULL is indistinguishable
    // from every other reason a DLL might fail to map.
    Info(L"  AppContainer ... %s",
         HasAppContainerReadAccess(dllPath)
             ? L"grants read + execute"
             : L"MISSING - the game cannot map this file");
}

}  // namespace launcher
