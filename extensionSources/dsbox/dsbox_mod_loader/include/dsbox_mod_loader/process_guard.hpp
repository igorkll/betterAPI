#pragma once

#include <string>

namespace dsbox_mod_loader
{
    enum class LaunchProtectionStatus
    {
        Allowed,               // no known anti-cheat service detected
        KnownAntiCheatDetected // see detail for which one
    };

    struct ProtectionCheckResult
    {
        LaunchProtectionStatus status = LaunchProtectionStatus::Allowed;
        std::wstring detail;
    };

    // Non-invasive, system-wide check: looks only at the list of
    // currently RUNNING processes on the machine for well-known
    // anti-cheat service executables (EasyAntiCheat, BattlEye), by the
    // same file names those vendors document themselves
    // (EasyAntiCheat.exe / EasyAntiCheat_EOS.exe, BEService.exe). It
    // never inspects the target game process itself (no OpenProcess,
    // no module walking) -- this is deliberately the least invasive
    // check we can do before even trying to launch anything.
    //
    // A result of KnownAntiCheatDetected means: do not attempt
    // injection at all. Launch the game exactly as Steam would have,
    // unmodified, and tell the user why.
    ProtectionCheckResult checkForKnownAntiCheat();

    // Remotely calls kernel32!SetDllDirectoryW(directory) inside the
    // already-created (suspended) target process, so the runtime DLL's
    // dependencies (DSBoxCore.dll, DSBoxConfig.dll, DSBoxIni.dll) resolve
    // from the runtime package directory without placing them in the game's
    // own directory.
    // This is a single, documented,
    // one-argument remote call -- same shape/risk profile as the
    // injection itself, not a search-order "hijack": it only ADDS a
    // directory we control to the target process's search path.
    //
    // Non-fatal by design: if this call is refused or fails, `detail`
    // explains why but the function returns false and the caller should
    // still attempt to load the runtime DLL; dependencies may already be
    // resolvable by another search path.
    bool setRemoteDllDirectory(void* processHandle, const std::wstring& directory, std::wstring& detail);

    // Loads the runtime DLL into an already-created (CREATE_SUSPENDED) process.
    //
    // This uses only documented, ordinary debugging-level OS facilities
    // -- nothing here touches kernel structures, patches code, or hides
    // itself. If the OS denies any step (ERROR_ACCESS_DENIED and
    // similar are the expected shape when the target is protected by an
    // anti-cheat/EAC-guarded process, a PPL, or insufficient
    // privileges), this returns false immediately with the Win32 error
    // text in `detail`. The caller MUST treat false as final: no retry,
    // no alternative injection technique -- resume the process
    // unmodified and warn the user.
    //
    // `processHandle` must have been opened/created with at least
    // PROCESS_VM_OPERATION, PROCESS_VM_WRITE, PROCESS_VM_READ and
    // PROCESS_CREATE_THREAD access (CreateProcess's returned process
    // handle already has all of these).
    bool loadRuntimeDllPolite(void* processHandle, const std::wstring& modulePath, std::wstring& detail);
}
