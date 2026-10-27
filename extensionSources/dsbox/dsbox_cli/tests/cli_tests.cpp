#include "test_framework.hpp"

#include <dsbox_cli/command_registry.hpp>
#include <dsbox_cli/tokenize.hpp>

#include <dsbox/communication_manager.hpp>

#include <stdexcept>

using namespace dsbox_cli;

namespace
{
    void tokenize_SplitsOnWhitespace()
    {
        auto tokens = tokenizeCommandLine("load  plugins/ping.so   true");

        DSBOX_CLI_CHECK_EQ(tokens.size(), 3u);
        DSBOX_CLI_CHECK_EQ(tokens[0], "load");
        DSBOX_CLI_CHECK_EQ(tokens[1], "plugins/ping.so");
        DSBOX_CLI_CHECK_EQ(tokens[2], "true");
    }

    void tokenize_EmptyLineProducesNoTokens()
    {
        DSBOX_CLI_CHECK(tokenizeCommandLine("   ").empty());
        DSBOX_CLI_CHECK(tokenizeCommandLine("").empty());
    }

    void tokenize_KeepsQuotedSpanAsOneToken()
    {
        auto tokens = tokenizeCommandLine(R"(say "hello world" done)");

        DSBOX_CLI_CHECK_EQ(tokens.size(), 3u);
        DSBOX_CLI_CHECK_EQ(tokens[1], "hello world");
    }

    void tokenize_UnterminatedQuoteTakesRestOfLine()
    {
        auto tokens = tokenizeCommandLine(R"(say "oops)");

        DSBOX_CLI_CHECK_EQ(tokens.size(), 2u);
        DSBOX_CLI_CHECK_EQ(tokens[1], "oops");
    }

    void registry_DispatchesThroughCommunicationManager()
    {
        dsbox::CommunicationManager comm;
        CommandRegistry registry(comm);

        registry.registerCommand("echo", "Echoes its arguments",
            [](CommandArgs args) -> CommandResult
            {
                CommandResult result;
                for (const auto& arg : args.args)
                    result.output += arg;
                return result;
            });

        auto handler = comm.getHandler<CommandSignature>("echo");
        DSBOX_CLI_CHECK(static_cast<bool>(handler));

        CommandArgs args;
        args.args = { "a", "b" };
        CommandResult result = handler(args);

        DSBOX_CLI_CHECK_EQ(result.output, "ab");
    }

    void registry_DuplicateNameThrows()
    {
        dsbox::CommunicationManager comm;
        CommandRegistry registry(comm);

        registry.registerCommand("ping", "First registration", [](CommandArgs) -> CommandResult { return {}; });

        bool threw = false;

        try
        {
            registry.registerCommand("ping", "Second registration", [](CommandArgs) -> CommandResult { return {}; });
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }

        DSBOX_CLI_CHECK(threw);
    }

    void registry_ListReturnsSortedDescriptions()
    {
        dsbox::CommunicationManager comm;
        CommandRegistry registry(comm);

        registry.registerCommand("zeta", "Z command", [](CommandArgs) -> CommandResult { return {}; });
        registry.registerCommand("alpha", "A command", [](CommandArgs) -> CommandResult { return {}; });

        auto list = registry.list();

        DSBOX_CLI_CHECK_EQ(list.size(), 2u);
        DSBOX_CLI_CHECK_EQ(list[0].first, "alpha");
        DSBOX_CLI_CHECK_EQ(list[1].first, "zeta");
    }
}

void runCliTests()
{
    dsbox_cli_tests::runTest("tokenize_SplitsOnWhitespace", tokenize_SplitsOnWhitespace);
    dsbox_cli_tests::runTest("tokenize_EmptyLineProducesNoTokens", tokenize_EmptyLineProducesNoTokens);
    dsbox_cli_tests::runTest("tokenize_KeepsQuotedSpanAsOneToken", tokenize_KeepsQuotedSpanAsOneToken);
    dsbox_cli_tests::runTest("tokenize_UnterminatedQuoteTakesRestOfLine", tokenize_UnterminatedQuoteTakesRestOfLine);
    dsbox_cli_tests::runTest("registry_DispatchesThroughCommunicationManager", registry_DispatchesThroughCommunicationManager);
    dsbox_cli_tests::runTest("registry_DuplicateNameThrows", registry_DuplicateNameThrows);
    dsbox_cli_tests::runTest("registry_ListReturnsSortedDescriptions", registry_ListReturnsSortedDescriptions);
}
