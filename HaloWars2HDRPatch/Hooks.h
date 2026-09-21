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

struct Config
{
    Mode mode = Mode::Hdr;

    // Semicolon-separated substrings, case insensitive; empty gates every movie.
    std::wstring gateFilter = L"MB_";

    bool log = false;
    bool logToFile = false;   // needs log
};

constexpr const wchar_t* kBinkModule = L"bink2winrt_x64.uni10.dll";

// The environment variable any injector can set instead of calling the export
// with an argument. Same key=value tokens, separated by '|' because an
// environment variable cannot hold a NUL.
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

// The one entry point both activation paths use. Idempotent and thread-safe:
// the first caller installs, later ones return the same result without
// repeating any of it. Returns non-zero on success.
DWORD InstallOnce(const wchar_t* args);

// --- logging --------------------------------------------------------------
// At 0 the call sites are removed outright, which constrains every caller:
// nothing with side effects in a Log argument, and no parameter that exists
// only to be logged.
#ifndef HW2HDRPATCH_LOGGING
#define HW2HDRPATCH_LOGGING 0
#endif
#if HW2HDRPATCH_LOGGING
using PfnLog = void(__fastcall*)(const wchar_t* format, ...);
extern PfnLog Log;
#else
#define Log(...) ((void)0)
#endif

// --- DXGI -----------------------------------------------------------------
bool InstallDxgiHooks();

// True only if the gate moved.
bool OpenGate();
bool CloseGate();

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
