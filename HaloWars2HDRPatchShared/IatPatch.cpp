// IatPatch.cpp - import address table patching.

#include "Hooks.h"

#include <algorithm>
#include <cstring>
#include <psapi.h>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace hw2hdr
{
namespace
{

std::vector<HMODULE> LoadedModules()
{
    std::vector<HMODULE> modules(512);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                            static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &needed))
    {
        return {};
    }

    modules.resize((std::min)(static_cast<size_t>(needed / sizeof(HMODULE)),
                              modules.size()));
    return modules;
}

bool NameMatches(const char* importName, const wchar_t* wanted)
{
    if (importName == nullptr)
    {
        return false;
    }

    wchar_t wide[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, importName, -1, wide, MAX_PATH);
    return _wcsicmp(wide, wanted) == 0;
}

// Raw pointers and PODs only: nothing needing unwinding, so the SEH wrapper
// below can enclose it.
size_t PatchModuleImportsUnguarded(HMODULE module, const wchar_t* moduleName,
                                   const ImportPatch* patches, size_t patchCount)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return 0;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0)
    {
        return 0;
    }

    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    size_t patched = 0;

    for (; descriptor->Name != 0; ++descriptor)
    {
        const char* importedModule =
            reinterpret_cast<const char*>(base + descriptor->Name);
        if (!NameMatches(importedModule, moduleName))
        {
            continue;
        }

        // OriginalFirstThunk keeps the names; FirstThunk is the live IAT.
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base
            + (descriptor->OriginalFirstThunk != 0 ? descriptor->OriginalFirstThunk
                                                   : descriptor->FirstThunk));
        auto* addressThunk =
            reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addressThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
            {
                continue;
            }

            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + nameThunk->u1.AddressOfData);
            const char* importName = reinterpret_cast<const char*>(byName->Name);

            for (size_t index = 0; index < patchCount; ++index)
            {
                const ImportPatch& patch = patches[index];
                if (strcmp(importName, patch.functionName) != 0)
                {
                    continue;
                }

                void** slot = reinterpret_cast<void**>(&addressThunk->u1.Function);
                if (*slot == patch.replacement)
                {
                    break;  // already ours
                }

                DWORD oldProtect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
                {
                    break;
                }

                // Blind store - the real function came from the export table,
                // never from this slot.
                *slot = patch.replacement;

                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                ++patched;
                break;
            }
        }
    }
    return patched;
}

// Pure, and [[maybe_unused]] because its only caller is a Log argument.
[[maybe_unused]] const wchar_t* LeafName(const wchar_t* path)
{
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash != nullptr ? slash + 1 : path;
}

size_t PatchModuleImports(HMODULE module, const wchar_t* moduleName,
                          const ImportPatch* patches, size_t patchCount)
{
    __try
    {
        return PatchModuleImportsUnguarded(module, moduleName, patches, patchCount);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Unmapped or half-initialised underneath us; faulting would take the
        // host down. Skipping it leaves its imports unpatched, so it is logged.
        [[maybe_unused]] const DWORD code = GetExceptionCode();
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        Log(L"[!] 0x%08X reading %s imports, skipped", code, LeafName(path));
        return 0;
    }
}

}  // namespace

size_t PatchImports(const wchar_t* moduleName, const ImportPatch* patches,
                    size_t patchCount)
{
    size_t total = 0;
    for (HMODULE module : LoadedModules())
    {
        // Never patch our own imports.
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        if (wcsstr(path, L"HaloWars2HDRPatch") != nullptr)
        {
            continue;
        }

        const size_t patched =
            PatchModuleImports(module, moduleName, patches, patchCount);
        if (patched > 0)
        {
            Log(L"    patched %zu import(s) of %s in %s", patched, moduleName,
                LeafName(path));
        }
        total += patched;
    }
    return total;
}

}  // namespace hw2hdr
