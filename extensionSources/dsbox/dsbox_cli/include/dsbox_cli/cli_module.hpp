#pragma once

#include <dsbox_cli/api.hpp>
#include <dsbox_cli/command.hpp>
#include <dsbox_cli/command_registry.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_io
{
    class IOModule;
}

namespace dsbox_cli
{
    // A minimal interactive command line: reads lines in run(), tokenizes
    // each into a command word plus arguments, and dispatches it through the same
    // dsbox::CommunicationManager every other cross-module call already
    // goes through: a command is just a dsbox::CommunicationManager
    // handler registered under its own name with CommandSignature.
    //
    // Modules add commands from their own onStart (never onInit -- see
    // module.hpp's lifecycle contract) via
    // context.get<CommandRegistry>()->registerCommand(...); this also
    // makes them show up in the built-in "help" command. Registering
    // directly on CommunicationManager<CommandSignature> instead still
    // works for dispatch, it just stays invisible to "help".
    //
    // Lifecycle:
    //   onInit  - registers the built-in commands (help/list/load/
    //             unload/reload/exit/quit) and publishes CommandRegistry
    //             into Context. Does not touch dsbox_io yet (per contract).
    //   onStart - looks up dsbox_io::IOModule. Every module's onInit has
    //             already run, so CommandRegistry exists before any
    //             module's onStart tries to register a command into it.
    //
    // Threading: run() reads and dispatches commands synchronously on its
    // caller's thread. A handler that blocks blocks the whole CLI until it
    // returns.
    //
    // Exit: run() returns after the built-in "exit"/"quit" command runs.
    class DSBOX_CLI_API CliModule final : public dsbox::IModule
    {
    public:
        std::string_view name() const override
        {
            return "dsbox.cli";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.io" };
        }

        void onInit(dsbox::Context& context) override;
        void onStart(dsbox::Context& context) override;

        // Reads and dispatches commands until "exit"/"quit" or stdin EOF.
        // Call from the host's main thread after ModuleManager::startAll().
        void run();

    private:
        void handleLine(std::string line);
        void registerBuiltinCommands(dsbox::Context& context);

        dsbox::Context* m_context = nullptr;
        dsbox_io::IOModule* m_io = nullptr;
        std::shared_ptr<CommandRegistry> m_registry;

        bool m_shouldExit = false;
    };
}
