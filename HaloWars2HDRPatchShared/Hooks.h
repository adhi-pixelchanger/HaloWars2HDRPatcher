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

    std::wstring gateFilter = L"MB_";

    bool log = _DEBUG_BOOL;
    bool logToFile = _DEBUG_BOOL;
};

constexpr const wchar_t* kBinkModule = L"bink2winrt_x64.uni10.dll";

#define HW2HDRPATCH_ENV_NAME L"HW2HDRPATCH"
constexpr const wchar_t* kEnvVarName = HW2HDRPATCH_ENV_NAME;

// Must match Launcher.h.
constexpr const wchar_t* kInstallEventPrefix = L"HaloWars2HDRPatch.InstallComplete.";

extern Config g_config;

// --- lifecycle ------------------------------------------------------------
void LoadConfig(const wchar_t* args);
void InitLog();
bool InstallHooks();

// Called from DllMain: must load nothing.
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

bool OpenGate();
bool CloseGate();

// --- IAT patching ---------------------------------------------------------
struct ImportPatch
{
    const char* functionName;
    void* replacement;
};

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
