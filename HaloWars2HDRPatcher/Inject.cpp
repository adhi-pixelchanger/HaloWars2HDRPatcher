// Inject.cpp - suspend, inject, resume, hand over the foreground.

#include "Launcher.h"

#include <psapi.h>
#include <sddl.h>
#include <userenv.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "userenv.lib")

namespace launcher {
namespace {

using PFN_NtSuspendProcess = LONG(NTAPI*)(HANDLE);
using PFN_NtResumeProcess = LONG(NTAPI*)(HANDLE);

template <typename T>
T GetNtdllExport(const char* name) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return nullptr;
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(ntdll, name)));
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

}  // namespace

bool SuspendProcess(HANDLE process) {
    static const auto fn = GetNtdllExport<PFN_NtSuspendProcess>("NtSuspendProcess");
    if (fn == nullptr) {
        Warn(L"NtSuspendProcess unavailable; injecting without suspending.");
        return false;
    }
    const LONG status = fn(process);
    if (status < 0) {
        Warn(L"NtSuspendProcess returned 0x%08X", status);
        return false;
    }
    return true;
}

bool ResumeProcess(HANDLE process) {
    static const auto fn = GetNtdllExport<PFN_NtResumeProcess>("NtResumeProcess");
    if (fn == nullptr) return false;
    const LONG status = fn(process);
    if (status < 0) {
        Fail(L"NtResumeProcess returned 0x%08X - the game is stuck suspended. "
             L"Kill it from Task Manager.", status);
        return false;
    }
    return true;
}

std::vector<std::wstring> EnumerateModules(HANDLE process) {
    std::vector<std::wstring> modules;
    if (process == nullptr) return modules;

    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (EnumProcessModulesEx(
                process, handles.data(),
                static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed,
                LIST_MODULES_ALL)) {
            if (needed > handles.size() * sizeof(HMODULE)) {
                handles.resize(needed / sizeof(HMODULE) + 16);
                continue;
            }
            const size_t count = needed / sizeof(HMODULE);
            for (size_t i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (GetModuleFileNameExW(process, handles[i], path, MAX_PATH) != 0) {
                    modules.emplace_back(path);
                }
            }
            return modules;
        }
        // A freshly created AppContainer process can return ERROR_PARTIAL_COPY
        // while its module list is still being built.
        Sleep(50);
    }
    return modules;
}

bool IsModuleLoaded(HANDLE process, const std::wstring& fileName) {
    const std::wstring wanted = ToLower(fileName);
    for (const auto& path : EnumerateModules(process)) {
        if (ToLower(FileNameOf(path)) == wanted) return true;
    }
    return false;
}

bool InjectDll(HANDLE process, const std::wstring& dllPath, DWORD* remoteResult) {
    if (remoteResult != nullptr) *remoteResult = 0;

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (remote == nullptr) {
        Fail(L"VirtualAllocEx failed: %lu", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        Fail(L"WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    // kernel32.dll is mapped at the same base in every process for a given boot,
    // so our local address of LoadLibraryW is valid in the target.
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
    if (loadLibrary == nullptr) {
        Fail(L"Could not resolve LoadLibraryW.");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (thread == nullptr) {
        const DWORD error = GetLastError();
        Fail(L"CreateRemoteThread failed: %lu%s", error,
             (error == ERROR_ACCESS_DENIED)
                 ? L" (access denied - antivirus or anti-cheat)"
                 : L"");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    if (remoteResult != nullptr) *remoteResult = exitCode;

    if (wait != WAIT_OBJECT_0) {
        Fail(L"Remote LoadLibraryW did not finish within 15 s.");
        return false;
    }

    // On x64 the HMODULE is truncated to 32 bits in the thread exit code, so a
    // non-zero value only tells us the load succeeded, not where it landed. The
    // authoritative check is the module list, done by the caller.
    if (exitCode == 0) {
        Fail(L"Remote LoadLibraryW returned NULL for %s.",
             FileNameOf(dllPath).c_str());
        return false;
    }

    return true;
}

// The remote LoadLibraryW thread's exit code cannot give us this: it is a DWORD
// and an x64 HMODULE does not fit.
HMODULE RemoteModuleBase(HANDLE process, const std::wstring& fileName)
{
    const std::wstring wanted = ToLower(fileName);

    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, handles.data(),
                              static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                              &needed, LIST_MODULES_ALL))
    {
        return nullptr;
    }

    const size_t count =
        (std::min)(static_cast<size_t>(needed / sizeof(HMODULE)), handles.size());
    for (size_t i = 0; i < count; ++i)
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameExW(process, handles[i], path, MAX_PATH) == 0)
        {
            continue;
        }
        if (ToLower(FileNameOf(path)) == wanted)
        {
            return handles[i];
        }
    }
    return nullptr;
}

bool RunRemoteInstall(HANDLE process, const std::wstring& dllPath,
                      const std::wstring& configArgs)
{
    const HMODULE remoteBase = RemoteModuleBase(process, FileNameOf(dllPath));
    if (remoteBase == nullptr)
    {
        Fail(L"  The payload is mapped but its base address could not be found.");
        return false;
    }

    // DONT_RESOLVE_DLL_REFERENCES runs no DllMain and binds no imports, so this
    // installs nothing into the launcher.
    const HMODULE local =
        LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local == nullptr)
    {
        Fail(L"  Could not map the payload locally to read its exports: %lu",
             GetLastError());
        return false;
    }

    auto localEntry = reinterpret_cast<BYTE*>(
        reinterpret_cast<void*>(GetProcAddress(local, "HaloWars2HDRPatchInstall")));
    const bool found = localEntry != nullptr;
    const SIZE_T rva =
        found ? static_cast<SIZE_T>(localEntry - reinterpret_cast<BYTE*>(local)) : 0;
    FreeLibrary(local);

    if (!found)
    {
        Fail(L"  The payload does not export HaloWars2HDRPatchInstall. It is not this "
             L"project's hook DLL, or it is out of date.");
        return false;
    }

    // Same image, so the same offset from whichever base each process used.
    auto remoteEntry = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(reinterpret_cast<BYTE*>(remoteBase) + rva));

    LPVOID remoteArgs = nullptr;
    if (!configArgs.empty())
    {
        // +1 for the terminator: the payload reads this as a C string.
        const SIZE_T bytes = (configArgs.size() + 1) * sizeof(wchar_t);
        remoteArgs = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
        SIZE_T written = 0;
        if (remoteArgs == nullptr
            || !WriteProcessMemory(process, remoteArgs, configArgs.data(), bytes,
                                   &written)
            || written != bytes)
        {
            // Not fatal: the installer treats a null list as "use the defaults".
            Warn(L"  Could not write the hook configuration into the game (%lu); "
                 L"the payload will use its compiled-in defaults.",
                 GetLastError());
            if (remoteArgs != nullptr)
            {
                VirtualFreeEx(process, remoteArgs, 0, MEM_RELEASE);
                remoteArgs = nullptr;
            }
        }
    }

    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, remoteEntry, remoteArgs, 0, nullptr);
    if (thread == nullptr)
    {
        Fail(L"  CreateRemoteThread for the installer failed: %lu", GetLastError());
        if (remoteArgs != nullptr) VirtualFreeEx(process, remoteArgs, 0, MEM_RELEASE);
        return false;
    }

    Info(L"  Install ........ running (host still suspended)...");

    // A watchdog only; nothing is sequenced by it.
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);

    // Only once the installer has returned: LoadConfig copies what it needs out
    // of this buffer, but it reads it on that thread.
    if (remoteArgs != nullptr && wait == WAIT_OBJECT_0)
    {
        VirtualFreeEx(process, remoteArgs, 0, MEM_RELEASE);
    }

    if (wait != WAIT_OBJECT_0)
    {
        // Left allocated on purpose - the thread may still be about to read it.
        Fail(L"  The installer thread did not finish within 30 s. Resuming the host "
             L"anyway; it will run unhooked. Check the payload log for the last "
             L"step it reached.");
        return false;
    }
    if (exitCode == 0)
    {
        Fail(L"  The installer ran but reported failure. See the hook log.");
        return false;
    }

    Info(L"  Install ........ hooks confirmed in place before resume");
    return true;
}

HANDLE CreateInstallEvent(HANDLE process, DWORD pid)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        Warn(L"  Install event .. cannot read the game's token: %lu", GetLastError());
        return nullptr;
    }

    alignas(TOKEN_APPCONTAINER_INFORMATION)
        BYTE buffer[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE] = {};
    DWORD length = 0;
    const BOOL queried =
        GetTokenInformation(token, TokenAppContainerSid, buffer, sizeof(buffer), &length);
    const DWORD queryError = GetLastError();
    CloseHandle(token);
    if (!queried)
    {
        Warn(L"  Install event .. cannot read the game's AppContainer: %lu", queryError);
        return nullptr;
    }

    std::wstring name = kInstallEventPrefix + std::to_wstring(pid);

    // Inside an AppContainer an unqualified name resolves to the container's own
    // directory, so the event has to be made there for the payload to find it.
    const PSID container =
        reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer;
    if (container != nullptr)
    {
        wchar_t path[MAX_PATH] = {};
        ULONG pathLength = 0;
        if (!GetAppContainerNamedObjectPath(nullptr, container, MAX_PATH, path,
                                            &pathLength))
        {
            Warn(L"  Install event .. GetAppContainerNamedObjectPath failed: %lu",
                 GetLastError());
            return nullptr;
        }
        name = std::wstring(path) + L"\\" + name;
    }

    // SetEvent from inside the sandbox must pass both halves of an AppContainer
    // access check - Everyone, and ALL APPLICATION PACKAGES - and a low label,
    // because the game runs at low integrity.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x100002;;;WD)(A;;0x100002;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1,
            &descriptor, nullptr))
    {
        Warn(L"  Install event .. cannot build its security descriptor: %lu",
             GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    const HANDLE event = CreateEventExW(&attributes, name.c_str(),
                                        CREATE_EVENT_MANUAL_RESET,
                                        SYNCHRONIZE | EVENT_MODIFY_STATE);
    const DWORD createError = GetLastError();
    LocalFree(descriptor);

    if (event == nullptr)
    {
        Warn(L"  Install event .. cannot create %s: %lu", name.c_str(), createError);
        return nullptr;
    }
    if (createError == ERROR_ALREADY_EXISTS)
    {
        ResetEvent(event);   // left by an earlier game that had this pid
    }
    return event;
}

// Only the foreground process may give the foreground away, which the launcher
// still is here. AllowSetForegroundWindow covers the case where the game's
// window does not exist yet.

namespace {

struct FindWindowContext
{
    DWORD pid = 0;
    HWND window = nullptr;
};

BOOL CALLBACK PickMainWindow(HWND window, LPARAM parameter)
{
    auto* context = reinterpret_cast<FindWindowContext*>(parameter);

    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != context->pid) return TRUE;
    if (!IsWindowVisible(window)) return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;

    context->window = window;
    return FALSE;
}

}  // namespace

void GiveForegroundTo(HANDLE process, DWORD pid)
{
    AllowSetForegroundWindow(pid);

    WaitForInputIdle(process, 10000);

    FindWindowContext context;
    context.pid = pid;
    EnumWindows(PickMainWindow, reinterpret_cast<LPARAM>(&context));

    if (context.window != nullptr)
    {
        SetForegroundWindow(context.window);
        Info(L"  Foreground ..... handed to the game's window");
    }
    else
    {
        Info(L"  Foreground ..... granted to the game (no window yet to raise)");
    }
}

}  // namespace launcher
