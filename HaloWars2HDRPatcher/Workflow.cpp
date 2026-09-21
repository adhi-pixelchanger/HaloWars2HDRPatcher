// Workflow.cpp - the whole run, start to finish, independent of the GUI.
//
// Ordered so a failure reports where it failed: everything checkable without a
// process first, then the process, then what needs it live, then the payload.

#include "Launcher.h"

#include <shobjidl_core.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

namespace launcher {
namespace {

// Not exported as a linkable symbol by every SDK.
const CLSID kClsidApplicationActivationManager = {
    0x45BA127D, 0x10A8, 0x46EA, {0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x78, 0x94, 0x3C}};

const DWORD kProcessAccess = PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                             PROCESS_VM_READ | PROCESS_SUSPEND_RESUME | SYNCHRONIZE;

struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() { if (ok) CoUninitialize(); }
};

// HW2HDRPATCH is user-global while it is set, so the window it exists for has to
// be as short as the activation itself and has to close on every path out.
struct EnvironmentConfigScope {
    bool active = false;

    EnvironmentConfigScope(bool wanted, const std::wstring& value) {
        if (!wanted) {
            // A previous env-mode run that was killed before its destructor ran
            // would otherwise still be set, and DllMain self-installs from it
            // before our installer argument is ever read - silently overriding
            // this launch. Clearing makes the normal path deterministic.
            if (HooksEnvironmentIsSet()) {
                Warn(L"  HW2HDRPATCH .... a stale value was set; cleared so it "
                     L"cannot override this launch.");
                ClearHooksEnvironment();
            }
            return;
        }

        active = SetHooksEnvironment(value);
        if (active) {
            Info(L"  HW2HDRPATCH .... set for activation: %s", value.c_str());
        } else {
            Warn(L"  HW2HDRPATCH .... could not be set; the payload will fall back "
                 L"to its compiled-in defaults.");
        }
    }

    ~EnvironmentConfigScope() {
        if (!active) return;
        Info(L"  HW2HDRPATCH .... %s",
             ClearHooksEnvironment() ? L"cleared" : L"COULD NOT BE CLEARED");
    }

    EnvironmentConfigScope(const EnvironmentConfigScope&) = delete;
    EnvironmentConfigScope& operator=(const EnvironmentConfigScope&) = delete;
};

// ---------------------------------------------------------------------------
// preflight
// ---------------------------------------------------------------------------

bool PreparePayload(const RunOptions& options, std::wstring& dllPath) {
    dllPath = AbsolutePath(options.dllPath);

    if (dllPath.empty() ||
        GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail(L"Payload not found: %s", dllPath.c_str());
        return false;
    }

    Info(L"  Hook config .... Mode=%s, GateFilter=\"%s\", Log=%s",
         HookModeName(options.hooks.mode), options.hooks.gateFilter.c_str(),
         options.hooks.log ? (options.hooks.logToFile ? L"file" : L"debugger")
                           : L"off");

    if (HasAppContainerReadAccess(dllPath)) {
        Info(L"  AppContainer .... can already read the payload; nothing changed.");
    } else {
        const bool granted = GrantAppContainerAccess(DirectoryOf(dllPath), true)
                             && GrantAppContainerAccess(dllPath, false);
        if (granted) {
            Info(L"  AppContainer .... was MISSING; granted read + execute on %s "
                 L"(inheritable).", DirectoryOf(dllPath).c_str());
        } else {
            Fail(L"  AppContainer .... is MISSING and could not be granted. The "
                 L"game will not be able to map the payload.");
            Fail(L"                    Fix by hand: icacls \"%s\" /grant "
                 L"\"*S-1-15-2-1:(OI)(CI)(RX)\"", DirectoryOf(dllPath).c_str());
        }
    }

    InspectPayload(dllPath);
    return true;
}

bool ResolveTarget(const RunOptions& options, std::wstring& fullName,
                   std::wstring& aumid) {
    Rule(L"target");

    if (!options.packageNeedle.empty()) {
        fullName = FindPackageFullName(options.packageNeedle);
    }

    if (fullName.empty() && options.aumidOverride.empty()) {
        Fail(L"  No installed package matched \"%s\".", options.packageNeedle.c_str());
        Fail(L"  Halo Wars 2's package is named after its codename - try "
             L"\"HoganThreshold\".");
        return false;
    }

    if (!fullName.empty()) Info(L"  Package ........ %s", fullName.c_str());

    aumid = options.aumidOverride;
    if (aumid.empty()) aumid = GetFirstAumid(fullName);

    if (aumid.empty()) {
        Fail(L"  Could not resolve an AUMID. Find it with:");
        Fail(L"    Get-StartApps | Where-Object Name -like '*Halo*'");
        return false;
    }
    Info(L"  AUMID .......... %s", aumid.c_str());

    if (!fullName.empty()) {
        const std::wstring install = GetPackageInstallPath(fullName);
        if (!install.empty()) Info(L"  Install path ... %s", install.c_str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// acquiring the process
// ---------------------------------------------------------------------------

DWORD ActivatePackage(const std::wstring& aumid, const std::wstring& args) {
    IApplicationActivationManager* manager = nullptr;
    HRESULT hr = CoCreateInstance(kClsidApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(hr) || manager == nullptr) {
        Fail(L"  CoCreateInstance(ApplicationActivationManager) failed: 0x%08X", hr);
        return 0;
    }

    DWORD pid = 0;
    hr = manager->ActivateApplication(aumid.c_str(),
                                      args.empty() ? nullptr : args.c_str(),
                                      AO_NOERRORUI, &pid);
    manager->Release();

    if (FAILED(hr) || pid == 0) {
        Fail(L"  ActivateApplication failed: 0x%08X", hr);
        if (hr == E_ACCESSDENIED) {
            Fail(L"  E_ACCESSDENIED almost always means this launcher is elevated.");
        }
        return 0;
    }
    return pid;
}

// ---------------------------------------------------------------------------
// verdict
// ---------------------------------------------------------------------------

std::wstring BuildVerdict(const RunResult& result, bool payloadLoaded) {
    const MitigationReport& m = result.mitigations;

    if (!result.launched) {
        return L"Did not get a process. Nothing about injection was tested.";
    }

    if (m.queried && m.microsoftSignedOnly) {
        return L"BLOCKED: the process runs with Code Integrity Guard "
               L"(MicrosoftSignedOnly). No unsigned DLL can be mapped into it by "
               L"any technique. This is the one case where \"it cannot be done\" "
               L"is actually true.";
    }

    if (payloadLoaded) {
        return L"SUCCESS: the payload is loaded in the game process and appears in "
               L"its module list.";
    }

    // Before the generic advice: a remote LoadLibraryW returning NULL carries
    // no error code back, so nothing else would name this.
    if (!result.payloadPath.empty()
        && !HasAppContainerReadAccess(result.payloadPath)) {
        return L"The payload did not load, and the AppContainer still cannot read "
               L"it. That is almost certainly the whole story: LoadLibraryW inside "
               L"the game fails with ACCESS_DENIED and returns NULL with no reason "
               L"attached. The grant above did not take - check that you own the "
               L"payload folder and that it is on an NTFS volume.";
    }

    if (m.queried && m.disableExtensionPoints) {
        return L"The payload did not load. Note that DisableExtensionPoints is set - "
               L"that blocks hook-based injectors, but it does not affect this path, "
               L"so the cause is elsewhere. Check the CreateRemoteThread result.";
    }

    return L"The payload did not load. Check the CreateRemoteThread result above, "
           L"the payload's architecture (must be x64), and any antivirus "
           L"quarantine.";
}

}  // namespace

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

RunResult Run(const RunOptions& options) {
    RunResult result;

    Rule(L"environment");
    const bool elevated = IsProcessElevated();
    Info(L"  Elevation ...... %s", elevated ? L"ELEVATED" : L"normal user");

    if (elevated) {
        Fail(L"  Windows refuses to activate Store apps from an elevated process.");
        Fail(L"  Close this and run it as a normal user. Elevation buys nothing "
             L"here: the game runs at Low integrity under your own account.");
        result.verdict = L"Cannot launch while elevated. Re-run as a normal user.";
        return result;
    }

    std::wstring dllPath;
    Rule(L"payload preparation");
    if (!PreparePayload(options, dllPath)) {
        result.verdict = L"Payload could not be prepared.";
        return result;
    }
    result.payloadPath = dllPath;

    std::wstring fullName;
    std::wstring aumid;
    if (!ResolveTarget(options, fullName, aumid)) {
        result.verdict = L"Target package could not be resolved.";
        return result;
    }

    ComScope com;
    if (!com.ok) {
        Fail(L"CoInitializeEx failed.");
        result.verdict = L"COM could not be initialised.";
        return result;
    }

    // Cleared by the destructor on every exit path, so a failure between here
    // and the resume cannot leave the variable behind for other processes.
    EnvironmentConfigScope envConfig(options.useEnvironmentConfig,
                                     BuildHooksArgs(options.hooks));

    // --- get a process ----------------------------------------------------
    Rule(L"activation");

    const DWORD pid = ActivatePackage(aumid, options.gameArgs);
    if (pid == 0) {
        result.verdict = BuildVerdict(result, false);
        return result;
    }

    result.pid = pid;
    result.launched = true;
    Info(L"  Process ........ pid %lu", pid);

    HANDLE process = OpenProcess(kProcessAccess, FALSE, pid);
    if (process == nullptr) {
        Fail(L"  OpenProcess failed: %lu", GetLastError());
        result.verdict = L"Could not open the game process for injection.";
        return result;
    }

    // Without this the host can create its swap chain and query GetDesc1
    // before the hooks exist.
    const bool suspended = SuspendProcess(process);
    if (suspended) Info(L"  Suspended ...... all game threads frozen");

    // --- diagnostics ------------------------------------------------------
    result.mitigations = QueryMitigations(process);
    ReportMitigations(result.mitigations);

    const std::wstring policyVerdict = InjectionVerdict(result.mitigations);
    if (!policyVerdict.empty()) {
        Blank();
        Info(L"  %s", policyVerdict.c_str());
    }

    // --- payload ----------------------------------------------------------
    Rule(L"payload injection");
    DWORD remoteResult = 0;
    InjectDll(process, dllPath, &remoteResult);

    bool payloadLoaded = IsModuleLoaded(process, FileNameOf(dllPath));
    result.injected = payloadLoaded;

    Info(L"  Remote LoadLibraryW returned 0x%08X", remoteResult);
    Info(L"  Module list check: %s",
         payloadLoaded ? L"PRESENT - the DLL is mapped"
                       : L"ABSENT - the DLL is not mapped");

    if (payloadLoaded && options.useEnvironmentConfig && envConfig.active) {
        // Deliberately no call into the payload: LoadLibraryW is all a foreign
        // injector would do, and DllMain has already started the install off the
        // environment variable. The cost is that there is no signal to wait on,
        // so the hooks are racing the host's startup from here rather than being
        // confirmed in place before the resume.
        Info(L"  Install ........ left to the payload's own HW2HDRPATCH path; no "
             L"signal to wait on, so it is not confirmed before resume.");
    } else if (payloadLoaded) {
        // envConfig.active is false here only if the variable could not be set,
        // in which case the environment route has nothing to read and would
        // install nothing at all.
        if (options.useEnvironmentConfig) {
            Warn(L"  Install ........ HW2HDRPATCH was not set, so that route is dead; "
                 L"falling back to the installer argument.");
        }
        result.injected =
            RunRemoteInstall(process, dllPath, BuildHooksArgs(options.hooks));
    }

    if (suspended) {
        ResumeProcess(process);
        Info(L"  Resumed ........ game threads running");
    }

    // --- wrap up ----------------------------------------------------------
    GiveForegroundTo(process, pid);

    CloseHandle(process);

    if (payloadLoaded && !fullName.empty()) {
        const std::wstring family = PackageFamilyFromFullName(fullName);
        if (!family.empty()) {
            Blank();
            Info(L"Hook log: %%LOCALAPPDATA%%\\Packages\\%s\\AC\\Temp\\"
                 L"HaloWars2HDRPatch.log",
                 family.c_str());
        }
    }

    result.verdict = BuildVerdict(result, payloadLoaded);
    return result;
}

}  // namespace launcher

namespace launcher {

RunOptions DefaultRunOptions() {
    RunOptions options;
    options.packageNeedle = L"HoganThreshold";
    options.dllPath = ExecutableDirectory() + L"\\HaloWars2HDRPatch.dll";

    options.hooks = HooksConfig();
    return options;
}

}  // namespace launcher
