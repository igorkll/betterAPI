// DSBoxModLoader -- a deliberately "dumb" shim for Steam's
// launch options:
//
//     "C:\path\to\DSBoxModLoader.exe" --main_exec=%command%
//
// optionally with an extra-arguments block for the GAME itself, order
// -independent relative to --main_exec=:
//
//     "...\DSBoxModLoader.exe" --ARG_PASS_BEGIN[[ -windowed -novid ]]ARG_PASS_END --main_exec=%command%
//
// It does exactly five things, in order:
//   1. Pull an optional --ARG_PASS_BEGIN[[...]]ARG_PASS_END block out of
//      its own raw command line and set it aside; pull the real game's
//      command line out of what's left (see command_line.hpp for why
//      this is done on the raw string rather than argv).
//   2. Non-invasively check whether a known anti-cheat service is
//      running; if so, launch the game completely unmodified.
//   3. Otherwise, create the game suspended and try one polite
//      LoadLibraryW injection of the sibling runtime DLL. Any failure
//      here (most commonly the target refusing the
//      attach) is final: we do not retry with another technique, we
//      just resume the game unmodified and tell the user why.
//   4. Resume the game and wait for it to exit, forwarding its exit
//      code -- so Steam's own process tracking (play time, overlay,
//      "stop" button) keeps working as if we were never here.
//
// This loader never patches any file belonging to the game or Steam,
// and never attempts to defeat a protection that refuses it -- see
// process_guard.hpp.
//
// No console window, by design: this is built as a GUI-subsystem
// (WIN32_EXECUTABLE) binary, so Windows never allocates a console for
// it the way it would for an ordinary console app -- a stray black
// window popping up next to someone's game on every launch would just
// be noise, and it has nothing to do with the GAME's own console (some
// games open their own via their own AllocConsole() call -- that is
// entirely theirs; we never touch it, never redirect its stdio, and
// never inherit handles into the child, see bInheritHandles below).
// All of this loader's own diagnostics go to a log file next to it
// (DSBoxModLoader.log) instead of stdout/stderr. If this loader is
// run manually from an existing terminal (e.g. while developing), it
// additionally attaches to that terminal so the same messages show up
// there too -- see Logger::Logger().

#include <dsbox_mod_loader/command_line.hpp>
#include <dsbox_mod_loader/process_guard.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    // Writes every logged line to the package log next to this
    // executable, and ALSO to the console if (and only if) this
    // process happens to already have one attached -- which, being a
    // GUI-subsystem binary, only happens if a developer launched it
    // from an existing terminal window via AttachConsole(). A normal
    // Steam launch has no parent console to attach to, so this is a
    // no-op there and no window is ever created.
    class Logger
    {
    public:
        explicit Logger(const std::filesystem::path& selfDir)
        {
            m_file.open(selfDir / L"DSBoxModLoader.log", std::ios::out | std::ios::app);
            m_hasConsole = ::AttachConsole(ATTACH_PARENT_PROCESS) != 0;

            if (m_hasConsole)
            {
                FILE* unused;
                freopen_s(&unused, "CONOUT$", "w", stdout);
                freopen_s(&unused, "CONOUT$", "w", stderr);
            }
        }

        void line(const std::wstring& text)
        {
            if (m_file.is_open())
                m_file << text << std::endl;

            if (m_hasConsole)
                std::fwprintf(stderr, L"[DSBoxModLoader] %ls\n", text.c_str());
        }

    private:
        std::wofstream m_file;
        bool m_hasConsole = false;
    };

    // Directory containing this loader executable and the package's bin/ tree.
    std::filesystem::path selfDirectory()
    {
        wchar_t buffer[MAX_PATH];
        DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);

        if (length == 0 || length == MAX_PATH)
            throw std::runtime_error("Failed to determine the loader's own path");

        return std::filesystem::path(buffer).parent_path();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    std::filesystem::path selfDir;

    try
    {
        selfDir = selfDirectory();
    }
    catch (const std::exception&)
    {
        return EXIT_FAILURE; // nowhere to even write a log
    }

    Logger logger(selfDir);

    std::wstring rawCommandLine = ::GetCommandLineW();

    // Step 1a: pull out --ARG_PASS_BEGIN[[...]]ARG_PASS_END, if present,
    // BEFORE looking for --main_exec= -- order-independent by design.
    auto argPass = dsbox_mod_loader::extractArgPassSection(rawCommandLine);

    if (argPass.malformed)
    {
        logger.line(
            L"--ARG_PASS_BEGIN[[ was found without a matching ]]ARG_PASS_END "
            L"in the launch options -- refusing to guess. Fix the launch "
            L"option and try again.");
        return EXIT_FAILURE;
    }

    // Step 1b: the real game's command line.
    std::wstring targetCommand = dsbox_mod_loader::extractMainExecCommand(argPass.remainder);

    if (targetCommand.empty())
    {
        logger.line(
            L"Usage: DSBoxModLoader.exe [--ARG_PASS_BEGIN[[ extra args ]]ARG_PASS_END] "
            L"--main_exec=<game command line>\n"
            L"This is meant to be set as a Steam launch option: "
            L"\"<path>\\DSBoxModLoader.exe\" --main_exec=%command%");
        return EXIT_FAILURE;
    }

    if (argPass.present && !argPass.extraArguments.empty())
    {
        logger.line(L"Passing extra arguments to the game: " + argPass.extraArguments);
        targetCommand += L" " + argPass.extraArguments;
    }

    auto leading = dsbox_mod_loader::parseLeadingExecutable(targetCommand);

    if (!leading.found)
    {
        logger.line(L"Could not determine the target executable from --main_exec");
        return EXIT_FAILURE;
    }

    std::filesystem::path targetExecutable = leading.executablePath;
    std::filesystem::path workingDirectory = targetExecutable.parent_path();

    logger.line(L"Target executable: " + targetExecutable.wstring());

    // Step 2: non-invasive protection check, BEFORE we create anything.
    auto protection = dsbox_mod_loader::checkForKnownAntiCheat();
    bool attemptInjection = true;

    if (protection.status == dsbox_mod_loader::LaunchProtectionStatus::KnownAntiCheatDetected)
    {
        logger.line(
            L"WARNING: " + protection.detail +
            L" -- refusing to inject into a protected game. "
            L"Launching the game unmodified instead.");
        attemptInjection = false;
    }

    // Step 3: launch the real game, suspended, via CreateProcess with
    // the ENTIRE target command line passed through verbatim (see
    // command_line.hpp) -- CreateProcess requires a mutable buffer for
    // lpCommandLine, hence the copy into a std::vector<wchar_t>.
    //
    // bInheritHandles=FALSE: the game never sees this loader's
    // stdio/log handles, and (symmetrically) never has its own
    // stdio/console touched by us -- if it opens its own console
    // window for its own logs, that is entirely its own business.
    std::vector<wchar_t> mutableCommandLine(targetCommand.begin(), targetCommand.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    BOOL created = ::CreateProcessW(
        nullptr,                 // the exe path is already embedded in the command line
        mutableCommandLine.data(),
        nullptr, nullptr,
        FALSE,                    // no handle inheritance -- see comment above
        CREATE_SUSPENDED,
        nullptr,                  // inherit our environment
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startupInfo, &processInfo);

    if (!created)
    {
        logger.line(L"Failed to launch the game (CreateProcess error " +
            std::to_wstring(::GetLastError()) + L")");
        return EXIT_FAILURE;
    }

    if (attemptInjection)
    {
        // The runtime uses the shared framework layout; its dependencies
        // remain in their normal subsystem directories under framework/.
        std::filesystem::path runtimeDll = selfDir / L"bin" / L"framework" / L"mod" / L"DSBoxModRuntime.dll";

        if (!std::filesystem::exists(runtimeDll))
        {
            logger.line(L"WARNING: " + runtimeDll.wstring() +
                L" not found -- launching without mods.");
        }
        else
        {
            // DSBoxModRuntime's direct DSBox dependencies live in the shared
            // framework/core directory. Non-fatal if refused: still try load.
            const std::filesystem::path frameworkCoreDir =
                selfDir / L"bin" / L"framework" / L"core";
            if (!::SetDllDirectoryW(frameworkCoreDir.c_str()))
            {
                logger.line(L"WARNING: could not set the loader's DLL search directory (error " +
                    std::to_wstring(::GetLastError()) + L"); runtime symbol lookup may fail.");
            }

            std::wstring dllDirDetail;
            if (!dsbox_mod_loader::setRemoteDllDirectory(
                    processInfo.hProcess, frameworkCoreDir.wstring(), dllDirDetail))
            {
                logger.line(L"WARNING: could not set the remote DLL search directory (" +
                    dllDirDetail + L"); injection may still fail if dependencies aren't found.");
            }

            std::wstring injectDetail;
            bool injected = dsbox_mod_loader::loadRuntimeDllPolite(
                processInfo.hProcess, runtimeDll.wstring(), injectDetail);

            if (injected)
            {
                logger.line(L"DSBox runtime DLL loaded: " + runtimeDll.wstring());
            }
            else
            {
                // Per policy: a refused injection is final. We do not
                // retry with any other technique -- we just let the
                // game run exactly as it would without us.
                logger.line(L"WARNING: injection was refused by the target process (" +
                    injectDetail + L"). Launching the game unmodified.");
            }
        }
    }

    // Step 4: resume and become a thin pass-through for the game's own
    // lifetime and exit code.
    ::ResumeThread(processInfo.hThread);
    ::CloseHandle(processInfo.hThread);

    ::WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 0;
    ::GetExitCodeProcess(processInfo.hProcess, &exitCode);
    ::CloseHandle(processInfo.hProcess);

    return static_cast<int>(exitCode);
}
