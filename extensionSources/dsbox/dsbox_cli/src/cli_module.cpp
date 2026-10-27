#include <dsbox_cli/cli_module.hpp>
#include <dsbox_cli/tokenize.hpp>

#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox/static_registry.hpp>

#include <dsbox_io/io_module.hpp>

#include <sstream>
#include <stdexcept>

namespace dsbox_cli
{
    namespace
    {
        std::string_view stateName(dsbox::ModuleState state)
        {
            switch (state)
            {
                case dsbox::ModuleState::Registered:  return "registered";
                case dsbox::ModuleState::Initialized: return "initialized";
                case dsbox::ModuleState::Started:     return "started";
                case dsbox::ModuleState::Stopped:     return "stopped";
                case dsbox::ModuleState::ShutDown:    return "shut down";
            }

            return "unknown";
        }
    }

    void CliModule::onInit(dsbox::Context& context)
    {
        m_context = &context;

        m_registry = std::make_shared<CommandRegistry>(context.comm());
        context.provide(m_registry);

        registerBuiltinCommands(context);

        context.comm().registerEvent<std::string>("io.stdin.line",
            [this](std::string line) { handleLine(std::move(line)); });
    }

    void CliModule::onStart(dsbox::Context& context)
    {
        m_io = dynamic_cast<dsbox_io::IOModule*>(context.modules()->find("dsbox.io"));

        if (!m_io)
            throw dsbox::ModuleError("dsbox.cli: dsbox.io did not start correctly");

        m_io->write("DSBox CLI ready. Type 'help' for a list of commands.\n");
    }

    void CliModule::run()
    {
        if (m_shouldExit)
            return;

        while (!m_shouldExit)
        {
            auto line = m_io->readStdinLine();
            if (!line)
                break;

            m_context->comm().emit<std::string>("io.stdin.line", std::move(*line));
        }
    }

    void CliModule::handleLine(std::string line)
    {
        std::vector<std::string> tokens = tokenizeCommandLine(line);

        if (tokens.empty())
            return;

        std::string name = std::move(tokens.front());
        CommandArgs args;
        args.args.assign(tokens.begin() + 1, tokens.end());

        auto handler = m_context->comm().getHandler<CommandSignature>(name);

        if (!handler)
        {
            m_io->writeError("Unknown command: '" + name + "' (try 'help')\n");
            return;
        }

        CommandResult result;

        try
        {
            result = handler(std::move(args));
        }
        catch (const std::exception& error)
        {
            m_io->writeError("Command '" + name + "' failed: " + error.what() + "\n");
            return;
        }
        catch (...)
        {
            m_io->writeError("Command '" + name + "' failed with an unknown exception\n");
            return;
        }

        if (!result.output.empty())
        {
            if (result.exitCode == 0)
                m_io->write(result.output + "\n");
            else
                m_io->writeError(result.output + "\n");
        }
    }

    void CliModule::registerBuiltinCommands(dsbox::Context& context)
    {
        m_registry->registerCommand("help", "List every available command",
            [this](CommandArgs) -> CommandResult
            {
                std::ostringstream out;
                out << "Available commands:\n";

                for (const auto& [cmdName, description] : m_registry->list())
                    out << "  " << cmdName << " - " << description << "\n";

                return { 0, out.str() };
            });

        m_registry->registerCommand("list", "List every registered module and its state",
            [&context](CommandArgs) -> CommandResult
            {
                std::ostringstream out;
                out << "Modules:\n";

                for (const auto& moduleName : context.modules()->moduleNames())
                {
                    out << "  " << moduleName
                        << " (" << stateName(context.modules()->stateOf(moduleName)) << ")\n";
                }

                return { 0, out.str() };
            });

        m_registry->registerCommand("load", "Load a module from a shared library: load <path>",
            [&context](CommandArgs args) -> CommandResult
            {
                if (args.args.empty())
                    return { 1, "usage: load <path>" };

                try
                {
                    auto& loaded = context.modules()->loadDynamic(args.args.front());
                    return { 0, "Loaded module '" + std::string(loaded.name()) + "'" };
                }
                catch (const std::exception& error)
                {
                    return { 1, std::string("load failed: ") + error.what() };
                }
            });

        m_registry->registerCommand("unload", "Unload a module by name: unload <name>",
            [&context](CommandArgs args) -> CommandResult
            {
                if (args.args.empty())
                    return { 1, "usage: unload <name>" };

                try
                {
                    context.modules()->unload(args.args.front());
                    return { 0, "Unloaded '" + args.args.front() + "'" };
                }
                catch (const std::exception& error)
                {
                    return { 1, std::string("unload failed: ") + error.what() };
                }
            });

        m_registry->registerCommand("reload", "Reload a module by name: reload <name>",
            [&context](CommandArgs args) -> CommandResult
            {
                if (args.args.empty())
                    return { 1, "usage: reload <name>" };

                try
                {
                    context.modules()->reload(args.args.front());
                    return { 0, "Reloaded '" + args.args.front() + "'" };
                }
                catch (const std::exception& error)
                {
                    return { 1, std::string("reload failed: ") + error.what() };
                }
            });

        auto exitHandler = [this](CommandArgs) -> CommandResult
        {
            m_shouldExit = true;

            return { 0, "Exiting..." };
        };

        m_registry->registerCommand("exit", "Exit the CLI and shut down", exitHandler);
        m_registry->registerCommand("quit", "Same as 'exit'", exitHandler);
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_cli::CliModule)
