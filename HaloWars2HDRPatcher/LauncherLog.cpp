// LauncherLog.cpp - log routing plus a few small helpers.

#include "Launcher.h"

#include <cstdarg>
#include <cstdio>

namespace launcher {
namespace {

LogSink g_sink = nullptr;
void* g_sinkUser = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;

void Emit(const wchar_t* prefix, const wchar_t* fmt, va_list args) {
    wchar_t body[1024];
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);

    wchar_t line[1152];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s%s", prefix, body);

    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");

    AcquireSRWLockExclusive(&g_logLock);
    if (g_sink != nullptr) g_sink(line, g_sinkUser);
    ReleaseSRWLockExclusive(&g_logLock);
}

}  // namespace

void SetLogSink(LogSink sink, void* user) {
    AcquireSRWLockExclusive(&g_logLock);
    g_sink = sink;
    g_sinkUser = user;
    ReleaseSRWLockExclusive(&g_logLock);
}

void Info(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit(L"", fmt, args);
    va_end(args);
}

void Warn(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit(L"[warn] ", fmt, args);
    va_end(args);
}

void Fail(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Emit(L"[error] ", fmt, args);
    va_end(args);
}

void Blank() { Info(L""); }

void Rule(const wchar_t* caption) {
    Info(L"");
    Info(L"---- %s ----", caption);
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L".";
    return DirectoryOf(path);
}

std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) return path;
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr) == 0) return path;
    return full;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring(L".") : path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

}  // namespace launcher
