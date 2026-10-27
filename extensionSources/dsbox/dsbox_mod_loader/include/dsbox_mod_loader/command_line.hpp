#pragma once

#include <string>

namespace dsbox_mod_loader
{
    // Everything this loader does with the raw OS command line lives
    // here as pure string functions (no process/OS calls), so the
    // parsing logic can be unit-tested without spawning anything.
    //
    // Why we re-parse the RAW command line (GetCommandLineW()) instead
    // of using argv/wargv: Steam's launch option
    //
    //     "C:\path\to\DSBoxModLoader.exe" --main_exec=%command%
    //
    // expands %command% textually, in place, into the single command
    // line string the OS hands to CreateProcess. If the real game's own
    // path or arguments contain quotes/spaces, re-splitting that string
    // into argv first (losing the original quoting) and reassembling it
    // later can corrupt it. Instead we find the "--main_exec=" marker in
    // the untouched raw string and keep everything after it as one
    // opaque blob, which we pass straight through to CreateProcess as
    // the child's lpCommandLine. We only peek at its first token (the
    // executable path) for logging/working-directory purposes.

    // Returns everything in `commandLine` after the first occurrence of
    // the literal marker "--main_exec=", with a single leading space (if
    // any) trimmed. Returns an empty string if the marker is not present
    // at all -- callers must treat that as "no target command given".
    std::wstring extractMainExecCommand(const std::wstring& commandLine);

    struct LeadingExecutable
    {
        std::wstring executablePath; // unquoted
        bool found = false;
    };

    // Parses just the first token (the program part) of a Windows
    // command line. Mirrors the special-cased rule CommandLineToArgvW
    // applies to argv[0] specifically: a leading '"' starts a quoted
    // run that ends at the NEXT '"' (no backslash-escaping inside it,
    // unlike every other argument), otherwise the token ends at the
    // first space. This is only used to learn the executable's path;
    // the full `command` string is still what gets passed to
    // CreateProcess verbatim.
    LeadingExecutable parseLeadingExecutable(const std::wstring& command);

    // Result of looking for an extra-arguments block in the loader's
    // own raw command line:
    //
    //     --ARG_PASS_BEGIN[[ -windowed -novid ]]ARG_PASS_END
    //
    // Everything between the markers is appended, verbatim, to the END
    // of the real game's command line (after whatever --main_exec=
    // already supplied) -- this is for per-launch flags someone wants
    // to hand the GAME itself (e.g. Steam launch-option style flags),
    // as opposed to config/modules.ini, which holds standing mod
    // configuration rather than one-off launch flags.
    //
    // Deliberately order-independent with respect to --main_exec=: the
    // block is located and REMOVED from the command line first (see
    // `remainder`), and --main_exec= is parsed from what's left. This
    // lets the ARG_PASS block appear before or after --main_exec=%command%
    // in Steam's launch options without either marker swallowing the
    // other.
    struct ArgPassResult
    {
        // True only when both a BEGIN and a matching END marker were
        // found (a well-formed block, possibly empty).
        bool present = false;

        // True when a BEGIN marker was found but no END marker followed
        // it -- a malformed launch option. Callers should treat this as
        // a hard error (refuse to guess what was meant) rather than
        // silently ignoring it.
        bool malformed = false;

        // Trimmed text between the markers. Only meaningful if
        // `present` is true.
        std::wstring extraArguments;

        // `commandLine` with the entire "--ARG_PASS_BEGIN[[...]]ARG_PASS_END"
        // span removed (the surrounding text is otherwise untouched).
        // If no BEGIN marker was found at all, this equals the input
        // unchanged. Always safe to use as the string to parse
        // --main_exec= out of next.
        std::wstring remainder;
    };

    ArgPassResult extractArgPassSection(const std::wstring& commandLine);
}
