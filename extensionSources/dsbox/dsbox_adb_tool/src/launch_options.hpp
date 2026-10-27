#pragma once

// Command-line parsing and small filesystem helpers shared by main.cpp
// and gui_mode.cpp. Split out of main.cpp (which used to
// contain all of this directly) purely to keep individual source files
// a manageable size once the GUI path was added -- no behavior change
// from the original dsbox_adb_tool.

#include <filesystem>
#include <string>
#include <vector>

namespace dsbox_adb_tool
{
    struct LaunchOptions
    {
        std::filesystem::path baseDir;

        // Where config/default/config.ini and config/config.ini are
        // resolved from. Equal to baseDir / "config" for --pathmode 1
        // (cwd) and 2 (explicit path), which keep full control over
        // their own layout -- but for --pathmode 0 (default, tied to
        // this executable's own location) this instead points at the
        // repo's consolidated top-level config/ tree, one level above
        // bin/executables/ (see docs/BUILD_LAYOUT.md and
        // dsbox_adb_tool/CMakeLists.txt's POST_BUILD step, which copies
        // the shipped default there, not next to the exe itself).
        std::filesystem::path configDir;

        std::vector<std::string> extraArgs; // leftover positional args: ".ini" module configs, or a single dynamic module path
    };

    // Throws std::runtime_error on a malformed --pathmode.
    LaunchOptions parseArgs(int argc, char* argv[]);

    // Directory containing the running executable itself, independent of
    // the current working directory.
    std::filesystem::path executableDirectory();

    // Reads the whole file into a string. Returns false (outText left
    // untouched) if the file couldn't be read.
    bool readFile(const std::filesystem::path& path, std::string& outText);

    // Like readFile(), but a missing file is not an error -- for
    // config/default/config.ini and config/config.ini, either of which
    // is legitimately absent.
    std::string readFileOrEmpty(const std::filesystem::path& path);

    bool endsWith(const std::string& text, const std::string& suffix);
}
