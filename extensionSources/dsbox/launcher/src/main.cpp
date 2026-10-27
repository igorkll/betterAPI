#include <dsbox/bootstrap_paths.hpp>
#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox/version.hpp>

#include <dsbox_config/module_config_loader.hpp>
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox_cli/cli_module.hpp>
#include <dsbox_io/io_module.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <limits.h>
    #include <unistd.h>
#endif

namespace
{
    bool endsWith(const std::string& text, const std::string& suffix)
    {
        return text.size() >= suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Reads the whole file into a string. This is the host's job -- the
    // dsbox_config library never touches the filesystem, it only parses
    // whatever text it is handed.
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

    // Like readFile(), but a missing file is not an error -- used for
    // config/default/config.ini and config/config.ini, either of which
    // is legitimately absent (a fresh install with no persisted
    // overrides yet, or an app that ships no defaults file at all).
    std::string readFileOrEmpty(const std::filesystem::path& path)
    {
        std::string text;

        if (std::filesystem::exists(path))
            readFile(path, text);

        return text;
    }

    // Directory containing the running executable itself, independent of
    // the current working directory (which is unreliable: shortcuts,
    // services, and other launch methods can start the process from
    // anywhere). This is --pathmode 0, the default.
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

    // Where /modules is resolved from, selected by --pathmode:
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

    // Where /config is resolved from. For --pathmode 0 (default) this is
    // the repo's consolidated top-level config/launcher/ tree, which now
    // lives at fin-out/config/ -- a sibling of fin-out/bin/, not of
    // bin/executables/ (see docs/BUILD_LAYOUT.md). Since this
    // executable's own directory is fin-out/bin/executables/, reaching
    // fin-out/config/ takes two ".." steps (executables -> bin ->
    // fin-out), not one. --pathmode 1 and 2 keep the simple
    // baseDir/"config" layout, since those already hand full control of
    // the directory structure to whoever passed them.
    std::filesystem::path resolveConfigDir(int pathMode, const std::filesystem::path& baseDir)
    {
        if (pathMode == 0)
            return baseDir / ".." / ".." / "config" / "launcher";

        return baseDir / "config";
    }

    struct LaunchOptions
    {
        std::filesystem::path baseDir;

        // See resolveConfigDir()'s own comment -- not simply
        // baseDir / "config" under the default --pathmode.
        std::filesystem::path configDir;

        std::vector<std::string> extraArgs; // leftover positional args
    };

    // Parses --pathmode (and its value(s)) out of argv; everything else
    // is left in extraArgs for the existing "load this .ini config or
    // .dll/.so directly" handling further down.
    LaunchOptions parseArgs(int argc, char* argv[])
    {
        int pathMode = 0;
        std::string explicitPath;
        std::vector<std::string> extraArgs;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];

            if (arg != "--pathmode")
            {
                extraArgs.push_back(std::move(arg));
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

        LaunchOptions options;
        options.baseDir = resolveBaseDir(pathMode, explicitPath);
        options.configDir = resolveConfigDir(pathMode, options.baseDir);
        options.extraArgs = std::move(extraArgs);
        return options;
    }

    // Loads every module declared in the config file at `path`. Returns
    // false only if the file itself couldn't be read; individual module
    // failures inside a valid config are logged and otherwise tolerated,
    // so one bad plugin entry doesn't take the whole host down.
    bool loadModuleConfig(
        dsbox::ModuleManager& modules,
        const std::filesystem::path& path,
        const std::filesystem::path& moduleBaseDir)
    {
        std::string iniText;

        if (!readFile(path, iniText))
        {
            std::fprintf(stderr, "Failed to read config '%s'\n", path.string().c_str());
            return false;
        }

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, iniText, moduleBaseDir.string());

        for (const auto& result : report.results)
        {
            if (!result.enabled)
            {
                std::printf("[config] '%s' skipped (disabled)\n", result.alias.c_str());
            }
            else if (result.loaded)
            {
                std::printf(
                    "[config] '%s' loaded module '%s' from %s\n",
                    result.alias.c_str(), result.moduleName.c_str(), result.path.c_str());
            }
            else
            {
                std::fprintf(
                    stderr, "[config] '%s' failed: %s\n",
                    result.alias.c_str(), result.error.c_str());
            }
        }

        return true;
    }

}

int main(int argc, char* argv[])
{
    // Must happen before ANY reference to a delay-loaded DSBox:: DLL
    // (see cmake/DSBoxDelayLoad.cmake) -- including the dsbox::version*()
    // calls the banner below makes into DSBoxCore.dll. Widens the
    // process's DLL search path to cover every bin/framework/<subsystem>/
    // subfolder; a no-op on non-Windows, where RPATH already handles
    // this at link time. Uses executableDirectory() (not --pathmode's
    // resolved baseDir, built further down) since --pathmode can point
    // config/modules somewhere else entirely while the DLLs themselves
    // always stay next to this binary.
    // bootstrapFrameworkSearchPaths() takes the FRAMEWORK ROOT
    // (fin-out/bin/framework/), not this executable's own directory --
    // see bootstrap_paths.hpp's own top comment.
    dsbox::bootstrapFrameworkSearchPaths((executableDirectory() / ".." / "framework").string());
    dsbox::preloadFrameworkModule("DSBoxIO.dll");
    dsbox::preloadFrameworkModule("DSBoxCli.dll");

    std::printf(
        "DSBox %u.%u.%u (%s)\n\n",
        dsbox::versionMajor(),
        dsbox::versionMinor(),
        dsbox::versionPatch(),
        dsbox::versionString()
    );

    LaunchOptions options;

    try
    {
        options = parseArgs(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }

    std::printf("Base directory: %s\n", options.baseDir.string().c_str());

    // configDir comes from LaunchOptions, not baseDir / "config" --
    // under the default --pathmode 0 it points at the repo's
    // consolidated top-level config/launcher/ tree instead (see
    // resolveConfigDir()'s own comment). modulesDir is unaffected:
    // plugin .dll/.so files are a separate concern from configuration
    // and keep their existing baseDir-relative layout.
    std::filesystem::path configDir = options.configDir;
    std::filesystem::path modulesDir = options.baseDir / "modules";

    // Layered persistent config: config/default/config.ini holds the
    // shipped defaults (read-only as far as this store is concerned);
    // config/config.ini holds only the deltas a user or a module has
    // explicitly changed at runtime. Neither file needs to exist yet.
    std::filesystem::path defaultsIniPath = configDir / "default" / "config.ini";
    std::filesystem::path overridesIniPath = configDir / "config.ini";

    auto configStore = std::make_shared<dsbox_config::PersistentConfigStore>(
        dsbox_config::PersistentConfigStore::load(
            readFileOrEmpty(defaultsIniPath),
            readFileOrEmpty(overridesIniPath)));

    dsbox::Context context;
    dsbox::ModuleManager modules(context);

    // Available to every module via context.get<PersistentConfigStore>().
    context.provide(configStore);

    // Pulls in every module registered via DSBOX_STATIC_MODULE in this
    // binary, e.g. dsbox_io::IOModule.
    modules.registerStaticModules();

    // Convention: <base>/config/modules.ini, if present, is loaded
    // automatically -- module paths inside it resolve against
    // <base>/modules. Its absence is not an error (e.g. a minimal setup
    // with only statically linked modules).
    std::filesystem::path defaultModulesIni = configDir / "modules.ini";

    if (std::filesystem::exists(defaultModulesIni))
    {
        if (!loadModuleConfig(modules, defaultModulesIni, modulesDir))
            return EXIT_FAILURE;
    }

    // Any remaining positional arguments are loaded IN ADDITION to the
    // above: a ".ini" argument is treated as another module config file
    // (read by the host, parsed by dsbox_config); anything else is
    // loaded directly as a single dynamic module path. Useful for ad hoc
    // testing without editing config/modules.ini.
    for (const auto& arg : options.extraArgs)
    {
        if (endsWith(arg, ".ini"))
        {
            std::filesystem::path path(arg);

            if (!loadModuleConfig(modules, path, path.parent_path()))
                return EXIT_FAILURE;

            continue;
        }

        try
        {
            auto& loaded = modules.loadDynamic(arg);
            std::printf("Loaded dynamic module '%.*s' from %s\n",
                static_cast<int>(loaded.name().size()), loaded.name().data(), arg.c_str());
        }
        catch (const std::exception& exception)
        {
            std::fprintf(stderr, "Failed to load '%s': %s\n", arg.c_str(), exception.what());
            return EXIT_FAILURE;
        }
    }

    try
    {
        modules.startAll();
    }
    catch (const dsbox::ModuleError& error)
    {
        std::fprintf(stderr, "Failed to start modules: %s\n", error.what());
        return EXIT_FAILURE;
    }

    if (auto* io = dynamic_cast<dsbox_io::IOModule*>(modules.find("dsbox.io")))
        io->write("DSBox launcher started.\n");
    else
        printf("IO NOT FOUND");


    // Read CLI commands on the main thread so input and shutdown share
    // one lifetime; a detached stdin reader would outlive stopAll().
    if (auto* cli = dynamic_cast<dsbox_cli::CliModule*>(modules.find("dsbox.cli")))
        cli->run();
    else
        printf("CLI NOT FOUND");

    modules.stopAll();

    // Explicit save, by design: PersistentConfigStore never writes to
    // disk on its own (see set()'s docs), so it's up to the host to
    // flush pending changes -- here, once at shutdown. A module
    // wanting immediate durability for a particular change is free to
    // ask the host to save sooner via whatever mechanism fits (an event
    // on the bus, a Context-provided callback, etc.) -- that policy
    // decision is intentionally left to the host, not baked into the
    // store itself.
    if (configStore->isDirty())
    {
        std::string overridesText = configStore->serializeOverrides();

        std::error_code ec;
        std::filesystem::create_directories(overridesIniPath.parent_path(), ec);

        std::ofstream out(overridesIniPath, std::ios::binary | std::ios::trunc);

        if (out)
        {
            out << overridesText;
            configStore->markSaved();
            std::printf("Saved config to %s\n", overridesIniPath.string().c_str());
        }
        else
        {
            std::fprintf(
                stderr, "Failed to write config to %s\n", overridesIniPath.string().c_str());
        }
    }

    return EXIT_SUCCESS;
}
