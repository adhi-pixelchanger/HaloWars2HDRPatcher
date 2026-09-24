// dllmain.cpp - entry point for HaloWars2HDRPatch.

#include "Hooks.h"

using namespace hw2hdr;

// LPTHREAD_START_ROUTINE signature: the launcher runs it via
// CreateRemoteThread and reads the result back as the exit code.
extern "C" __declspec(dllexport) DWORD WINAPI HaloWars2HDRPatchInstall(LPVOID args)
{
    // Written into this process by the launcher; null if anything else loaded
    // us, in which case InstallOnce falls back to HW2HDRPATCH and then defaults.
    return InstallOnce(static_cast<const wchar_t*>(args));
}

namespace
{

// The install must not run under the loader lock - will cause loader deadlock
DWORD WINAPI SelfInstallThread(LPVOID)
{
    InstallOnce(nullptr);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        if (EnvironmentConfigPresent())
        {
            const HANDLE thread =
                CreateThread(nullptr, 0, &SelfInstallThread, nullptr, 0, nullptr);
            if (thread != nullptr)
            {
                CloseHandle(thread);
            }
        }
    }
    return TRUE;
}
