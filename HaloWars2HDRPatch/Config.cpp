// Config.cpp - configuration, logging, and wiring the modes together.
//
// Config is one flat '|'-separated key=value string rather than a struct, so the
// EXE and DLL share no layout. Byte-identical whichever way it arrives:
//
//   1. the LPVOID handed to HaloWars2HDRPatchInstall, written into this process
//      by our launcher
//   2. the HW2HDRPATCH environment variable
//   3. compiled-in defaults
//
// Form (2) is what lets any injector drive this DLL with nothing but
// LoadLibrary: DllMain self-installs when the variable is present.
//
// The DLL cannot write next to itself: its directory is granted read/execute
// only. GetTempPathW in a packaged process resolves to the package's own
// writable temp folder.

#include "Hooks.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <fstream>

namespace hw2hdr
{

Config g_config;

namespace
{

ULONGLONG g_startTick = 0;

const wchar_t* ModeName(Mode mode)
{
    return mode == Mode::Sdr ? L"SDR (whole session)" : L"HDR (movies only)";
}

// Caps guard against a caller that is not our launcher handing us something
// that is not one of our lists.
constexpr size_t kMaxTokens = 32;
constexpr size_t kMaxTokenChars = 512;

bool KeyIs(const wchar_t* token, const wchar_t* key, const wchar_t** value)
{
    const size_t length = wcslen(key);
    if (_wcsnicmp(token, key, length) != 0 || token[length] != L'=')
    {
        return false;
    }
    *value = token + length + 1;
    return true;
}

bool TruthyValue(const wchar_t* value)
{
    return *value == L'1' || _wcsicmp(value, L"true") == 0;
}

void ParseArgs(const wchar_t* args)
{
    for (size_t index = 0; index < kMaxTokens && *args != L'\0'; ++index)
    {
        const wchar_t* separator = wcschr(args, L'|');
        const size_t length =
            separator != nullptr ? static_cast<size_t>(separator - args)
                                 : wcsnlen(args, kMaxTokenChars);
        if (length >= kMaxTokenChars)
        {
            break;   // not a list we wrote
        }

        // Copied out so the value is NUL-terminated for the comparisons below.
        wchar_t token[kMaxTokenChars];
        wmemcpy(token, args, length);
        token[length] = L'\0';

        const wchar_t* value = nullptr;
        if (KeyIs(token, L"Mode", &value))
        {
            g_config.mode = _wcsicmp(value, L"SDR") == 0 ? Mode::Sdr : Mode::Hdr;
        }
        else if (KeyIs(token, L"GateFilter", &value))
        {
            g_config.gateFilter = value;
        }
        else if (KeyIs(token, L"Log", &value))
        {
            g_config.log = TruthyValue(value);
        }
        else if (KeyIs(token, L"LogToFile", &value))
        {
            g_config.logToFile = TruthyValue(value);
        }

        if (separator == nullptr)
        {
            break;
        }
        args = separator + 1;
    }

    // logToFile is meaningless without log, whatever order the keys arrived in.
    if (!g_config.log)
    {
        g_config.logToFile = false;
    }
}

}  // namespace

bool EnvironmentConfigPresent()
{
    return GetEnvironmentVariableW(kEnvVarName, nullptr, 0) > 1;
}

void LoadConfig(const wchar_t* args)
{
    g_startTick = GetTickCount64();

    if (args != nullptr)
    {
        ParseArgs(args);
        return;
    }

    // No explicit list: the environment holds the identical text, then defaults.
    const DWORD length = GetEnvironmentVariableW(kEnvVarName, nullptr, 0);
    if (length <= 1 || length > kMaxTokens * kMaxTokenChars)
    {
        return;
    }

    std::vector<wchar_t> buffer(length, L'\0');
    if (GetEnvironmentVariableW(kEnvVarName, buffer.data(), length) == length - 1)
    {
        ParseArgs(buffer.data());
    }
}


#if HW2HDRPATCH_LOGGING

SRWLOCK g_logLock = SRWLOCK_INIT;
std::unique_ptr<std::wofstream> g_logFile;

// The compile-time switch removes this whole block and every call site.
void LogNoop(const wchar_t* /*format*/, ...) {}
void LogImpl(const wchar_t* format, ...);

PfnLog Log = LogNoop;

void InitLog()
{
    if (g_startTick == 0)
    {
        g_startTick = GetTickCount64();
    }

    // LogImpl never re-checks g_config.log - that is the point of swapping
    // the pointer rather than branching per call.
    if (!g_config.log)
    {
        return;
    }

    Log = LogImpl;

    if (!g_config.logToFile)
    {
        return;  // debugger output only
    }

    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0)
    {
        Log(L"[!] GetTempPath failed (%lu); file logging is off for this session.",
            GetLastError());
        return;
    }
    const auto logPath = std::wstring(temp) + L"HaloWars2HDRPatch.log";
    g_logFile.reset(new std::wofstream(logPath));
}

void LogImpl(const wchar_t* format, ...)
{
    AutoSRWLockExclusive logLock(g_logLock);

    wchar_t body[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, format, args);
    va_end(args);

    const double seconds =
        static_cast<double>(GetTickCount64() - g_startTick) / 1000.0;
    wchar_t line[1400];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%9.3f] %s\r\n", seconds, body);

    OutputDebugStringW(line);

    if (g_logFile && g_logFile->is_open())
    {
        *g_logFile << line;
    }
}

void LogFlush()
{
    AutoSRWLockExclusive logLock(g_logLock);
    if (g_logFile && g_logFile->is_open())
    {
        g_logFile->flush();
    }
}

#else  // !HW2HDRPATCH_LOGGING

// g_config.log/logToFile are dead in this build.
void InitLog()
{
}

#endif  // HW2HDRPATCH_LOGGING

SRWLOCK g_installLock = SRWLOCK_INIT;

// Both activation paths land here, and either may be the only one, or both may
// fire for the same process: our launcher passing a list while the environment
// variable also happens to be set, or a third-party injector that both
// LoadLibrary's us and calls the export. The lock makes concurrent callers wait
// rather than race, and the flag makes the later ones no-ops returning the same
// answer.
DWORD InstallOnce(const wchar_t* args)
{    
    AutoSRWLockExclusive installLock(g_installLock);

    static bool done = false;
    static DWORD result = 0;

    if (done)
    {
        return result;
    }
    done = true;

    LoadConfig(args);
    InitLog();
    Log(L"==== attached to pid %lu (%s) ====", GetCurrentProcessId(),
        args != nullptr ? L"config from installer argument"
                        : L"config from " HW2HDRPATCH_ENV_NAME L" / defaults");

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
    {
        Log(L"host: %s", exePath);
    }

    if (!InstallHooks())
    {
        Log(L"[!] hook installation failed - the host is unmodified");
        LogFlush();
        return result;   // still 0
    }

    Log(L"install complete");
    LogFlush();
    result = 1;
    return result;
}

bool InstallHooks()
{
    Log(L"Mode ........... %s", ModeName(g_config.mode));

    if (!InstallDxgiHooks())
    {
        return false;
    }

    if (g_config.mode == Mode::Sdr)
    {
        OpenGate();
        Log(L"    ...and stays open for the whole session.");
        return true;
    }

    Log(L"Bink module .... %s", kBinkModule);
    Log(L"Gate filter .... \"%s\"",
        g_config.gateFilter.empty() ? L"(every movie)" : g_config.gateFilter.c_str());

    const HMODULE bink = GetModuleHandleW(kBinkModule);
    Log(L"Bink module %s at startup.",
        bink != nullptr ? L"is loaded" : L"is NOT yet loaded");

    if (!InstallBinkHooks())
    {
        Log(L"[!] Nothing imports %s by name, so HDR mode has no movie "
            L"boundaries to gate on and will do nothing.",
            kBinkModule);
        if (bink != nullptr)
        {
            Log(L"[!] The module IS loaded but nothing imports it by name. The host "
                L"is binding Bink dynamically through GetProcAddress, which an "
                L"import-table patch cannot reach.");
        }
    }

    return true;
}

}  // namespace hw2hdr
