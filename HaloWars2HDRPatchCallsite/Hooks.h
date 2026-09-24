// Hooks.h - shared declarations for HaloWars2HDRPatch.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>

#include <string>

namespace hw2hdr
{

enum class Mode
{
    Sdr,
    Hdr,
};

#if defined(_DEBUG)
#define _DEBUG_BOOL true
#else
#define _DEBUG_BOOL false
#endif

struct Config
{
    Mode mode = Mode::Hdr;

    // Semicolon-separated substrings, case insensitive; empty gates every movie.
    std::wstring gateFilter = L"MB_";

    bool log = _DEBUG_BOOL;
    bool logToFile = _DEBUG_BOOL;   // needs log
};

constexpr const wchar_t* kBinkModule = L"bink2winrt_x64.uni10.dll";

// Environment variable in an alternate injector is used
#define HW2HDRPATCH_ENV_NAME L"HW2HDRPATCH"
constexpr const wchar_t* kEnvVarName = HW2HDRPATCH_ENV_NAME;

extern Config g_config;

// --- lifecycle ------------------------------------------------------------
// args is the '|'-separated key=value string, written into this process by the
// launcher. Null falls back to the environment variable, then to compiled-in
// defaults.
void LoadConfig(const wchar_t* args);
void InitLog();
bool InstallHooks();

// True if the environment variable is set and non-empty. Cheap enough to call
// from DllMain: it reads the process environment block and loads nothing.
bool EnvironmentConfigPresent();

DWORD InstallOnce(const wchar_t* args);

// --- logging --------------------------------------------------------------
#ifndef HW2HDRPATCH_LOGGING
#define HW2HDRPATCH_LOGGING _DEBUG_BOOL
#endif
#if HW2HDRPATCH_LOGGING
using PfnLog = void(*)(const wchar_t* format, ...);
extern PfnLog Log;
void LogFlush();
#else
#define Log(...) ((void)0)
#define LogFlush() ((void)0)
#endif

// --- DXGI -----------------------------------------------------------------
bool InstallDxgiHooks();

// True only if the gate moved.
bool OpenGate();
bool CloseGate();

// --- call-site patching ---------------------------------------------------
// The host's own call site is rewritten, not the vtable behind it: a vtable
// belongs to dxgi.dll and is shared with its internal calls, while a call site
// belongs to one caller. Install-time only, while the launcher has the host's
// threads frozen.
enum class CallSiteOutcome
{
    Patched,
    NotFound,
    Ambiguous,
    OutOfReach,
    ProtectFailed,
};

struct VirtualCall
{
    size_t slot;            // vtable index the host calls through
    size_t anchorSlot;      // another slot used nearby, to tell two sites apart
    const void* nearSite;   // search around this address only, or null for the
                            // whole image
    const IID* anchorIid;   // the interface the host asks for beside the call,
                            // which is what actually identifies it
};

// replacement must have the method's own signature; site reports where it
// landed, which is what a later nearSite is.
CallSiteOutcome PatchVirtualCall(const VirtualCall& call, void* replacement,
                                 const void** site);
const wchar_t* Describe(CallSiteOutcome outcome);

// --- IAT patching (HDR mode only) -----------------------------------------
// No "original" out-parameter: the real addresses come from GetProcAddress
// against the pinned module, not from the slot being overwritten.
struct ImportPatch
{
    const char* functionName;
    void* replacement;
};

// Walks every loaded module once, applying the whole set. Called during install
// only: by then the loader has resolved the host's static imports, which is
// where both targets live.
size_t PatchImports(const wchar_t* moduleName, const ImportPatch* patches,
                    size_t patchCount);

bool InstallBinkHooks();

struct AutoSRWLockExclusive
{
    SRWLOCK& lock;
    AutoSRWLockExclusive(SRWLOCK& _lock) : lock(_lock) { AcquireSRWLockExclusive(&lock); }\
    ~AutoSRWLockExclusive() { ReleaseSRWLockExclusive(&lock); }
};

}  // namespace hw2hdr
