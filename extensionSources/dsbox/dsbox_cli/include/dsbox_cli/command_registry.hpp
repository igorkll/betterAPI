#pragma once

#include <dsbox_cli/api.hpp>
#include <dsbox_cli/command.hpp>

#include <dsbox/communication_manager.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dsbox_cli
{
    // Published into Context by CliModule::onInit (context.get<CommandRegistry>()),
    // so any module can register a command in its own onStart without
    // needing to depend on CliModule directly -- only on this small type.
    //
    // registerCommand() is a thin wrapper around
    // CommunicationManager::registerHandler<CommandSignature>: actual
    // dispatch always goes through the bus (CliModule looks commands up
    // there, not through this class), this only adds a description
    // alongside the registration so "help" has something to print.
    // Calling CommunicationManager::registerHandler<CommandSignature>
    // directly instead of going through here still works for execution
    // -- the command just won't show up in "help".
    //
    // Thread safety: registerCommand()/describe() are safe to call
    // concurrently from multiple threads (own mutex, separate from
    // CommunicationManager's).
    class DSBOX_CLI_API CommandRegistry
    {
    public:
        explicit CommandRegistry(dsbox::CommunicationManager& comm)
            : m_comm(comm)
        {
        }

        CommandRegistry(const CommandRegistry&) = delete;
        CommandRegistry& operator=(const CommandRegistry&) = delete;

        // Throws std::runtime_error if `name` is already registered --
        // same rejection CommunicationManager::registerHandler itself
        // would throw, just reached one line higher.
        template<typename Callable>
        void registerCommand(std::string name, std::string description, Callable&& handler)
        {
            m_comm.registerHandler<CommandSignature>(name, std::forward<Callable>(handler));

            std::lock_guard<std::mutex> lock(m_mutex);
            m_descriptions.emplace(std::move(name), std::move(description));
        }

        // Snapshot of every command registered THROUGH THIS registry,
        // sorted by name, for "help" to print. Commands registered
        // directly on CommunicationManager (bypassing this class) are
        // invisible here by design -- see the class comment.
        std::vector<std::pair<std::string, std::string>> list() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::vector<std::pair<std::string, std::string>> result(
                m_descriptions.begin(), m_descriptions.end());

            std::sort(result.begin(), result.end());
            return result;
        }

    private:
        dsbox::CommunicationManager& m_comm;

        mutable std::mutex m_mutex;
        std::unordered_map<std::string, std::string> m_descriptions;
    };
}
