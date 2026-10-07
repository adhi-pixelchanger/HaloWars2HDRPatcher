// dllmain.cpp - entry point for HaloWars2HDRPatch.

#include "Hooks.h"

using namespace hw2hdr;

extern "C" __declspec(dllexport) DWORD WINAPI HaloWars2HDRPatchInstall(LPVOID args)
{
    return InstallOnce(static_cast<const wchar_t*>(args));
}

namespace
{

// InstallOnce deadlocks under the loader lock.
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
