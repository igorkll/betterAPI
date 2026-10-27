#include <dsbox_mod_loader/process_guard.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dsbox_mod_loader
{
    namespace
    {
        std::wstring toLower(std::wstring text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
            return text;
        }

        std::wstring formatLastError(const wchar_t* what, DWORD code)
        {
            wchar_t* messageBuffer = nullptr;

            ::FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);

            std::wstring result = what;
            result += L" failed (error ";
            result += std::to_wstring(code);
            result += L"): ";
            result += messageBuffer ? messageBuffer : L"unknown error";

            if (messageBuffer)
                ::LocalFree(messageBuffer);

            return result;
        }

        // Known anti-cheat *service/watchdog* process names, as shipped
        // and documented by the respective vendors. Listing a process
        // name publicly known to belong to a well-known anti-cheat
        // product is not a bypass of anything -- it is how we know to
        // back off instead of touching it.
        const wchar_t* kKnownAntiCheatProcesses[] = {
            L"easyanticheat.exe",
            L"easyanticheat_eos.exe",
            L"beservice.exe",
            L"bedaisy.exe",
        };

        // Shared by setRemoteDllDirectory() and loadRuntimeDllPolite():
        // both are "call this one-argument kernel32 function remotely,
        // passing it a wide string" -- VirtualAllocEx the string into
        // the target, CreateRemoteThread with the function's address as
        // the entry point and the string's remote address as the sole
        // argument, wait for it, clean up.
        bool callRemoteStringFunction(
            HANDLE process,
            const char* functionName,
            const std::wstring& argument,
            DWORD& outExitCode,
            std::wstring& detail)
        {
            HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");

            if (!kernel32)
            {
                detail = formatLastError(L"GetModuleHandleW(kernel32.dll)", ::GetLastError());
                return false;
            }

            // kernel32.dll is loaded at the same address in every
            // process on a given Windows session (it is always among
            // the first DLLs mapped), so resolving a function's address
            // in OUR process and calling it as the remote thread's
            // entry point in the target process is valid for any plain
            // kernel32 export taking a single pointer-sized argument.
            auto function =
                reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(kernel32, functionName));

            if (!function)
            {
                detail = formatLastError(L"GetProcAddress", ::GetLastError());
                return false;
            }

            size_t bytes = (argument.size() + 1) * sizeof(wchar_t);

            LPVOID remoteBuffer = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE);

            if (!remoteBuffer)
            {
                detail = formatLastError(L"VirtualAllocEx", ::GetLastError());
                return false;
            }

            if (!::WriteProcessMemory(process, remoteBuffer, argument.c_str(), bytes, nullptr))
            {
                detail = formatLastError(L"WriteProcessMemory", ::GetLastError());
                ::VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
                return false;
            }

            HANDLE remoteThread =
                ::CreateRemoteThread(process, nullptr, 0, function, remoteBuffer, 0, nullptr);

            if (!remoteThread)
            {
                detail = formatLastError(L"CreateRemoteThread", ::GetLastError());
                ::VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
                return false;
            }

            ::WaitForSingleObject(remoteThread, INFINITE);
            ::GetExitCodeThread(remoteThread, &outExitCode);

            ::CloseHandle(remoteThread);
            ::VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
            return true;
        }
    }

    ProtectionCheckResult checkForKnownAntiCheat()
    {
        ProtectionCheckResult result;

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

        if (snapshot == INVALID_HANDLE_VALUE)
            return result; // can't enumerate -> don't block on an unknown

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (::Process32FirstW(snapshot, &entry))
        {
            do
            {
                std::wstring name = toLower(entry.szExeFile);

                for (const wchar_t* known : kKnownAntiCheatProcesses)
                {
                    if (name == known)
                    {
                        result.status = LaunchProtectionStatus::KnownAntiCheatDetected;
                        result.detail = L"Detected running anti-cheat process: " + name;
                        ::CloseHandle(snapshot);
                        return result;
                    }
                }
            } while (::Process32NextW(snapshot, &entry));
        }

        ::CloseHandle(snapshot);
        return result;
    }

    bool setRemoteDllDirectory(void* processHandle, const std::wstring& directory, std::wstring& detail)
    {
        DWORD exitCode = 0;
        bool called = callRemoteStringFunction(
            static_cast<HANDLE>(processHandle), "SetDllDirectoryW", directory, exitCode, detail);

        if (!called)
            return false;

        if (exitCode == 0) // SetDllDirectoryW returns BOOL
        {
            detail = L"SetDllDirectoryW returned FALSE inside the target process";
            return false;
        }

        return true;
    }

    bool loadRuntimeDllPolite(void* processHandle, const std::wstring& modulePath, std::wstring& detail)
    {
        DWORD exitCode = 0;
        bool called = callRemoteStringFunction(
            static_cast<HANDLE>(processHandle), "LoadLibraryW", modulePath, exitCode, detail);

        if (!called)
            return false;

        if (exitCode == 0)
        {
            detail = L"LoadLibraryW returned NULL inside the target process";
            return false;
        }

        // Load a local copy only to resolve the exported initializer and
        // calculate its RVA. The loader's own process already has the
        // framework/core directory in its normal DLL search path.
        HMODULE localImage = ::LoadLibraryExW(modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!localImage)
        {
            detail = formatLastError(L"LoadLibraryExW(local runtime image)", ::GetLastError());
            return false;
        }

        FARPROC localInitializer = ::GetProcAddress(localImage, "DSBoxRuntimeInitialize");
        if (!localInitializer)
        {
            detail = L"The runtime DLL does not export DSBoxRuntimeInitialize";
            ::FreeLibrary(localImage);
            return false;
        }

        DWORD processId = ::GetProcessId(static_cast<HANDLE>(processHandle));
        HANDLE snapshot = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            detail = formatLastError(L"CreateToolhelp32Snapshot(target modules)", ::GetLastError());
            ::FreeLibrary(localImage);
            return false;
        }

        const std::wstring wantedName = toLower(std::filesystem::path(modulePath).filename().wstring());
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        HMODULE remoteImage = nullptr;
        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (toLower(std::filesystem::path(entry.szExePath).filename().wstring()) == wantedName)
                {
                    remoteImage = entry.hModule;
                    break;
                }
            } while (::Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);

        if (!remoteImage)
        {
            detail = L"The runtime DLL was not present in the target module list after LoadLibraryW";
            ::FreeLibrary(localImage);
            return false;
        }

        const auto rva = reinterpret_cast<std::uintptr_t>(localInitializer) -
            reinterpret_cast<std::uintptr_t>(localImage);
        auto remoteInitializer = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<std::uintptr_t>(remoteImage) + rva);
        HANDLE remoteThread = ::CreateRemoteThread(
            static_cast<HANDLE>(processHandle), nullptr, 0, remoteInitializer, nullptr, 0, nullptr);
        if (!remoteThread)
        {
            detail = formatLastError(L"CreateRemoteThread(DSBoxRuntimeInitialize)", ::GetLastError());
            ::FreeLibrary(localImage);
            return false;
        }

        ::WaitForSingleObject(remoteThread, INFINITE);
        DWORD initializeResult = 0;
        BOOL gotResult = ::GetExitCodeThread(remoteThread, &initializeResult);
        ::CloseHandle(remoteThread);
        ::FreeLibrary(localImage);
        if (!gotResult)
        {
            detail = formatLastError(L"GetExitCodeThread(DSBoxRuntimeInitialize)", ::GetLastError());
            return false;
        }
        if (initializeResult == 0)
        {
            detail = L"DSBoxRuntimeInitialize returned FALSE inside the target process";
            return false;
        }
        return true;
    }
}
