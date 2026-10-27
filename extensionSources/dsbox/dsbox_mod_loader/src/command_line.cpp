#include <dsbox_mod_loader/command_line.hpp>

namespace dsbox_mod_loader
{
    namespace
    {
        const wchar_t* kMainExecMarker = L"--main_exec=";
        const size_t kMainExecMarkerLength = 12; // wcslen(kMainExecMarker)

        const wchar_t* kArgPassBegin = L"--ARG_PASS_BEGIN[[";
        const size_t kArgPassBeginLength = 18; // wcslen(kArgPassBegin)

        const wchar_t* kArgPassEnd = L"]]ARG_PASS_END";
        const size_t kArgPassEndLength = 14; // wcslen(kArgPassEnd)

        std::wstring trim(const std::wstring& text)
        {
            size_t begin = text.find_first_not_of(L" \t");

            if (begin == std::wstring::npos)
                return L"";

            size_t end = text.find_last_not_of(L" \t");
            return text.substr(begin, end - begin + 1);
        }
    }

    std::wstring extractMainExecCommand(const std::wstring& commandLine)
    {
        size_t pos = commandLine.find(kMainExecMarker);

        if (pos == std::wstring::npos)
            return L"";

        size_t start = pos + kMainExecMarkerLength;

        if (start < commandLine.size() && commandLine[start] == L' ')
            ++start;

        return commandLine.substr(start);
    }

    LeadingExecutable parseLeadingExecutable(const std::wstring& command)
    {
        LeadingExecutable result;

        size_t i = 0;
        size_t n = command.size();

        // Skip leading whitespace, if any.
        while (i < n && command[i] == L' ')
            ++i;

        if (i >= n)
            return result; // empty command -> not found

        std::wstring token;

        if (command[i] == L'"')
        {
            // Quoted run: everything up to the NEXT quote, verbatim
            // (argv[0] does not interpret backslashes specially).
            ++i;
            size_t end = command.find(L'"', i);

            if (end == std::wstring::npos)
            {
                token = command.substr(i); // unterminated quote: take the rest
            }
            else
            {
                token = command.substr(i, end - i);
            }
        }
        else
        {
            size_t end = command.find(L' ', i);

            if (end == std::wstring::npos)
                token = command.substr(i);
            else
                token = command.substr(i, end - i);
        }

        if (token.empty())
            return result;

        result.executablePath = std::move(token);
        result.found = true;
        return result;
    }

    ArgPassResult extractArgPassSection(const std::wstring& commandLine)
    {
        ArgPassResult result;
        result.remainder = commandLine;

        size_t beginPos = commandLine.find(kArgPassBegin);

        if (beginPos == std::wstring::npos)
            return result; // no block at all -- not an error

        size_t contentStart = beginPos + kArgPassBeginLength;
        size_t endPos = commandLine.find(kArgPassEnd, contentStart);

        if (endPos == std::wstring::npos)
        {
            result.malformed = true;
            return result; // BEGIN without a matching END
        }

        result.present = true;
        result.extraArguments = trim(commandLine.substr(contentStart, endPos - contentStart));

        size_t blockEnd = endPos + kArgPassEndLength;
        result.remainder = commandLine.substr(0, beginPos) + commandLine.substr(blockEnd);
        return result;
    }
}
