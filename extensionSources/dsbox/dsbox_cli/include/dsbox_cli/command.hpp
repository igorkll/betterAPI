#pragma once

#include <string>
#include <vector>

namespace dsbox_cli
{
    // The payload/return types every CLI command handler agrees on.
    // Registered on dsbox::CommunicationManager under the command's own
    // name (e.g. "ping"), via CommandSignature -- see CommandRegistry.
    //
    // args does NOT include the command word itself, only what follows
    // it on the input line (already tokenized -- see CliModule's line
    // parsing for the quoting rules).
    struct CommandArgs
    {
        std::vector<std::string> args;
    };

    // exitCode is informational only right now (nothing built on top of
    // it yet besides CliModule printing output either way); nonzero is
    // the convention for "the command considers itself to have failed".
    // output is a single human-readable block of text, written via
    // dsbox_io::IOModule -- CliModule appends a trailing '\n' itself, so
    // handlers should not add one.
    struct CommandResult
    {
        int exitCode = 0;
        std::string output;
    };

    using CommandSignature = CommandResult(CommandArgs);
}
