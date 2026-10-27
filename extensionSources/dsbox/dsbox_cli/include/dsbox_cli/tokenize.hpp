#pragma once

#include <dsbox_cli/api.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace dsbox_cli
{
    // Whitespace-separated tokens, with double-quoted spans kept as a
    // single token (so an argument containing spaces can be written
    // "like this"). No escaping of embedded quotes -- this is meant for
    // a human typing commands, not a full shell grammar. An unterminated
    // trailing quote just takes the rest of the line as-is rather than
    // erroring, since dropping half the input on a typo is worse than
    // being lenient about it.
    DSBOX_CLI_API std::vector<std::string> tokenizeCommandLine(std::string_view line);
}
