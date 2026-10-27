#include "launch_options.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <limits.h>
    #include <unistd.h>
#endif

namespace dsbox_adb_tool
{
    bool endsWith(const std::string& text, const std::string& suffix)
    {
        return text.size() >= suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool readFile(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
            return false;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        outText = buffer.str();
        return true;
    }

    std::string readFileOrEmpty(const std::filesystem::path& path)
    {
        std::string text;

        if (std::filesystem::exists(path))
            readFile(path, text);

        return text;
    }

    std::filesystem::path executableDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);

        if (length == 0 || length == MAX_PATH)
            throw std::runtime_error("Failed to determine the executable's own path");

        return std::filesystem::path(buffer).parent_path();
#else
        char buffer[PATH_MAX];
        ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

        if (length == -1)
            throw std::runtime_error(
                "Failed to determine the executable's own path (/proc/self/exe)");

        buffer[length] = '\0';
        return std::filesystem::path(buffer).parent_path();
#endif
    }

    namespace
    {
        // Where /modules is resolved from, selected by --pathmode --
        // same three modes as launcher:
        //   0 (default) -- the directory containing this executable.
        //   1           -- the current working directory.
        //   2 <path>    -- an explicit directory given on the command line.
        std::filesystem::path resolveBaseDir(int pathMode, const std::string& explicitPath)
        {
            switch (pathMode)
            {
                case 0:
                    return executableDirectory();
                case 1:
                    return std::filesystem::current_path();
                case 2:
                    return std::filesystem::path(explicitPath);
                default:
                    throw std::runtime_error("Unreachable: invalid path mode");
            }
        }

        // Where /config is resolved from. For --pathmode 0 this is the
        // repo's consolidated top-level config/dsbox_adb_tool/ tree, which
        // now lives at fin-out/config/ -- a sibling of fin-out/bin/, not of
        // bin/executables/. Since this executable's own directory is
        // fin-out/bin/executables/, reaching fin-out/config/ takes two
        // ".." steps (executables -> bin -> fin-out), not one (see
        // LaunchOptions::configDir's own doc comment). --pathmode 1 and 2
        // keep the simple baseDir/"config" layout, since those already hand
        // full control of the directory structure to whoever passed them.
        std::filesystem::path resolveConfigDir(int pathMode, const std::filesystem::path& baseDir)
        {
            if (pathMode == 0)
                return baseDir / ".." / ".." / "config" / "dsbox_adb_tool";

            return baseDir / "config";
        }
    }

    LaunchOptions parseArgs(int argc, char* argv[])
    {
        int pathMode = 0;
        std::string explicitPath;
        LaunchOptions options;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];

            if (arg == "--cli")
            {
                throw std::runtime_error(
                    "The interactive console was removed from dsbox_adb_tool; launch without --cli to use the GUI");
            }

            if (arg != "--pathmode")
            {
                options.extraArgs.push_back(std::move(arg));
                continue;
            }

            if (i + 1 >= argc)
                throw std::runtime_error("--pathmode requires a value (0, 1, or 2)");

            std::string value = argv[++i];

            if (value == "0")
            {
                pathMode = 0;
            }
            else if (value == "1")
            {
                pathMode = 1;
            }
            else if (value == "2")
            {
                pathMode = 2;

                if (i + 1 >= argc)
                    throw std::runtime_error("--pathmode 2 requires a path argument");

                explicitPath = argv[++i];
            }
            else
            {
                throw std::runtime_error(
                    "Invalid --pathmode value '" + value + "' (expected 0, 1, or 2)");
            }
        }

        options.baseDir = resolveBaseDir(pathMode, explicitPath);
        options.configDir = resolveConfigDir(pathMode, options.baseDir);
        return options;
    }
}
