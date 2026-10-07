// Launcher.h - shared declarations for the launcher.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

namespace launcher {

// --- logging --------------------------------------------------------------
using LogSink = void (*)(const wchar_t* line, void* user);
void SetLogSink(LogSink sink, void* user);
void Info(const wchar_t* fmt, ...);
void Warn(const wchar_t* fmt, ...);
void Fail(const wchar_t* fmt, ...);
void Blank();
void Rule(const wchar_t* caption);

// --- app model ------------------------------------------------------------
std::vector<std::wstring> EnumerateLaunchablePackages();
std::wstring FindPackageFullName(const std::wstring& needle);
std::wstring PackageFamilyFromFullName(const std::wstring& fullName);
std::wstring GetFirstAumid(const std::wstring& packageFullName);
std::wstring GetPackageInstallPath(const std::wstring& packageFullName);

// --- security -------------------------------------------------------------
bool GrantAppContainerAccess(const std::wstring& path, bool isDirectory);
bool HasAppContainerReadAccess(const std::wstring& path);

// --- process control ------------------------------------------------------
bool SuspendProcess(HANDLE process);
bool ResumeProcess(HANDLE process);
bool InjectDll(HANDLE process, const std::wstring& dllPath, DWORD* remoteResult);

void GiveForegroundTo(HANDLE process, DWORD pid);

bool RunRemoteInstall(HANDLE process, const std::wstring& dllPath,
                      const std::wstring& configArgs);

// Must match kInstallEventPrefix in HaloWars2HDRPatchShared's Hooks.h.
constexpr const wchar_t* kInstallEventPrefix = L"HaloWars2HDRPatch.InstallComplete.";

HANDLE CreateInstallEvent(HANDLE process, DWORD pid);
std::vector<std::wstring> EnumerateModules(HANDLE process);
bool IsModuleLoaded(HANDLE process, const std::wstring& fileName);

// --- diagnostics ----------------------------------------------------------

struct MitigationReport {
    bool queried = false;
    bool microsoftSignedOnly = false;
    bool storeSignedOnly = false;
    bool signatureMitigationOptIn = false;
    bool noRemoteImages = false;
    bool noLowMandatoryLabelImages = false;
    bool preferSystem32Images = false;
    bool disableExtensionPoints = false;
    bool prohibitDynamicCode = false;
    bool allowThreadOptOut = false;
    bool cfgEnabled = false;
    bool cfgStrict = false;
};

MitigationReport QueryMitigations(HANDLE process);
void ReportMitigations(const MitigationReport& report);

bool AcgBlocksPayload(const MitigationReport& report, const std::wstring& dllPath);

std::wstring InjectionVerdict(const MitigationReport& report,
                              const std::wstring& dllPath);

enum class DllArch { Unknown, X86, X64, Arm64 };
DllArch QueryDllArchitecture(const std::wstring& path);
const wchar_t* DllArchName(DllArch arch);

enum class SignatureState { NotSigned, Trusted, Untrusted, Error };
SignatureState QueryAuthenticode(const std::wstring& path, DWORD* status);
const wchar_t* SignatureStateName(SignatureState state);

void InspectPayload(const std::wstring& dllPath);

// --- misc -----------------------------------------------------------------
bool IsProcessElevated();
std::wstring ExecutableDirectory();
std::wstring AbsolutePath(const std::wstring& path);
std::wstring DirectoryOf(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);

// --- hook DLL configuration -----------------------------------------------
enum class HookMode
{
    Sdr,
    Hdr,
};

struct HooksConfig
{
    HookMode mode = HookMode::Hdr;
    std::wstring gateFilter = L"MB_";

    bool log = false;
    bool logToFile = false;
};

const wchar_t* HookModeName(HookMode mode);

std::wstring BuildHooksArgs(const HooksConfig& config);

bool HooksEnvironmentIsSet();
bool SetHooksEnvironment(const std::wstring& value);
bool ClearHooksEnvironment();

// --- payload strategy -----------------------------------------------------
// Values are dropdown indices: keep kStrategies in enum order.
enum class Strategy
{
    Vtable,
    Callsite,
    QI,
};

constexpr Strategy kStrategies[] = {Strategy::Vtable, Strategy::Callsite, Strategy::QI};
constexpr Strategy kDefaultStrategy = Strategy::Callsite;

const wchar_t* StrategyName(Strategy strategy);

std::wstring StrategyDllPath(Strategy strategy);

bool StrategyOfPath(const std::wstring& path, Strategy* strategy);

// --- orchestration --------------------------------------------------------
struct RunOptions {
    std::wstring packageNeedle;
    std::wstring aumidOverride;
    std::wstring dllPath;
    std::wstring gameArgs;
    HooksConfig hooks;

    bool useEnvironmentConfig = false;
};

struct RunResult {
    bool launched = false;
    bool injected = false;
    DWORD pid = 0;
    std::wstring payloadPath;
    MitigationReport mitigations;
    std::wstring verdict;
};

RunOptions DefaultRunOptions();

RunResult Run(const RunOptions& options);

}  // namespace launcher
