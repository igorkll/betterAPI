#include "test_framework.hpp"

#include <dsbox_mod_loader/command_line.hpp>

using namespace dsbox_mod_loader;

namespace
{
    void extractMainExecCommand_findsMarker()
    {
        std::wstring raw = L"\"C:\\DSBox\\DSBoxModLoader.exe\" --main_exec=C:\\Game\\Game.exe -arg1 -arg2";
        std::wstring result = extractMainExecCommand(raw);
        DSBOX_MOD_LOADER_CHECK_EQ(result, L"C:\\Game\\Game.exe -arg1 -arg2");
    }

    void extractMainExecCommand_handlesQuotedGamePath()
    {
        std::wstring raw =
            L"\"C:\\DSBox\\DSBoxModLoader.exe\" --main_exec=\"C:\\Program Files\\Game\\Game.exe\" -arg1";
        std::wstring result = extractMainExecCommand(raw);
        DSBOX_MOD_LOADER_CHECK_EQ(result, L"\"C:\\Program Files\\Game\\Game.exe\" -arg1");
    }

    void extractMainExecCommand_missingMarkerReturnsEmpty()
    {
        std::wstring raw = L"\"C:\\DSBox\\DSBoxModLoader.exe\"";
        std::wstring result = extractMainExecCommand(raw);
        DSBOX_MOD_LOADER_CHECK(result.empty());
    }

    void parseLeadingExecutable_plainPath()
    {
        auto result = parseLeadingExecutable(L"C:\\Game\\Game.exe -arg1 -arg2");
        DSBOX_MOD_LOADER_CHECK(result.found);
        DSBOX_MOD_LOADER_CHECK_EQ(result.executablePath, L"C:\\Game\\Game.exe");
    }

    void parseLeadingExecutable_quotedPathWithSpaces()
    {
        auto result = parseLeadingExecutable(L"\"C:\\Program Files\\Game\\Game.exe\" -arg1");
        DSBOX_MOD_LOADER_CHECK(result.found);
        DSBOX_MOD_LOADER_CHECK_EQ(result.executablePath, L"C:\\Program Files\\Game\\Game.exe");
    }

    void parseLeadingExecutable_noArguments()
    {
        auto result = parseLeadingExecutable(L"C:\\Game\\Game.exe");
        DSBOX_MOD_LOADER_CHECK(result.found);
        DSBOX_MOD_LOADER_CHECK_EQ(result.executablePath, L"C:\\Game\\Game.exe");
    }

    void parseLeadingExecutable_emptyInput()
    {
        auto result = parseLeadingExecutable(L"");
        DSBOX_MOD_LOADER_CHECK(!result.found);
    }

    void extractArgPassSection_findsBlockAfterMainExec()
    {
        std::wstring raw =
            L"\"loader.exe\" --main_exec=C:\\Game\\Game.exe "
            L"--ARG_PASS_BEGIN[[ -windowed -novid ]]ARG_PASS_END";
        auto result = extractArgPassSection(raw);
        DSBOX_MOD_LOADER_CHECK(result.present);
        DSBOX_MOD_LOADER_CHECK(!result.malformed);
        DSBOX_MOD_LOADER_CHECK_EQ(result.extraArguments, L"-windowed -novid");
        DSBOX_MOD_LOADER_CHECK_EQ(result.remainder, L"\"loader.exe\" --main_exec=C:\\Game\\Game.exe ");

        // The markers themselves must never end up anywhere that gets
        // forwarded to the game -- neither in the extracted arguments
        // nor left behind in the remainder.
        DSBOX_MOD_LOADER_CHECK(result.extraArguments.find(L"ARG_PASS") == std::wstring::npos);
        DSBOX_MOD_LOADER_CHECK(result.remainder.find(L"ARG_PASS") == std::wstring::npos);
    }

    void extractArgPassSection_findsBlockBeforeMainExec()
    {
        std::wstring raw =
            L"\"loader.exe\" --ARG_PASS_BEGIN[[ -windowed -novid ]]ARG_PASS_END "
            L"--main_exec=C:\\Game\\Game.exe -own-arg";
        auto result = extractArgPassSection(raw);
        DSBOX_MOD_LOADER_CHECK(result.present);
        DSBOX_MOD_LOADER_CHECK_EQ(result.extraArguments, L"-windowed -novid");

        std::wstring target = extractMainExecCommand(result.remainder);
        DSBOX_MOD_LOADER_CHECK_EQ(target, L"C:\\Game\\Game.exe -own-arg");
    }

    void extractArgPassSection_noBlockIsNotAnError()
    {
        std::wstring raw = L"\"loader.exe\" --main_exec=C:\\Game\\Game.exe";
        auto result = extractArgPassSection(raw);
        DSBOX_MOD_LOADER_CHECK(!result.present);
        DSBOX_MOD_LOADER_CHECK(!result.malformed);
        DSBOX_MOD_LOADER_CHECK_EQ(result.remainder, raw);
    }

    void extractArgPassSection_missingEndMarkerIsMalformed()
    {
        std::wstring raw = L"\"loader.exe\" --ARG_PASS_BEGIN[[ -windowed --main_exec=C:\\Game\\Game.exe";
        auto result = extractArgPassSection(raw);
        DSBOX_MOD_LOADER_CHECK(!result.present);
        DSBOX_MOD_LOADER_CHECK(result.malformed);
    }

    void extractArgPassSection_emptyBlock()
    {
        std::wstring raw = L"\"loader.exe\" --ARG_PASS_BEGIN[[]]ARG_PASS_END --main_exec=C:\\Game\\Game.exe";
        auto result = extractArgPassSection(raw);
        DSBOX_MOD_LOADER_CHECK(result.present);
        DSBOX_MOD_LOADER_CHECK(result.extraArguments.empty());
    }
}

void runCommandLineTests()
{
    dsbox_mod_loader_tests::runTest(
        "extractMainExecCommand_findsMarker", extractMainExecCommand_findsMarker);
    dsbox_mod_loader_tests::runTest(
        "extractMainExecCommand_handlesQuotedGamePath", extractMainExecCommand_handlesQuotedGamePath);
    dsbox_mod_loader_tests::runTest(
        "extractMainExecCommand_missingMarkerReturnsEmpty",
        extractMainExecCommand_missingMarkerReturnsEmpty);
    dsbox_mod_loader_tests::runTest("parseLeadingExecutable_plainPath", parseLeadingExecutable_plainPath);
    dsbox_mod_loader_tests::runTest(
        "parseLeadingExecutable_quotedPathWithSpaces", parseLeadingExecutable_quotedPathWithSpaces);
    dsbox_mod_loader_tests::runTest("parseLeadingExecutable_noArguments", parseLeadingExecutable_noArguments);
    dsbox_mod_loader_tests::runTest("parseLeadingExecutable_emptyInput", parseLeadingExecutable_emptyInput);
    dsbox_mod_loader_tests::runTest(
        "extractArgPassSection_findsBlockAfterMainExec", extractArgPassSection_findsBlockAfterMainExec);
    dsbox_mod_loader_tests::runTest(
        "extractArgPassSection_findsBlockBeforeMainExec", extractArgPassSection_findsBlockBeforeMainExec);
    dsbox_mod_loader_tests::runTest(
        "extractArgPassSection_noBlockIsNotAnError", extractArgPassSection_noBlockIsNotAnError);
    dsbox_mod_loader_tests::runTest(
        "extractArgPassSection_missingEndMarkerIsMalformed", extractArgPassSection_missingEndMarkerIsMalformed);
    dsbox_mod_loader_tests::runTest("extractArgPassSection_emptyBlock", extractArgPassSection_emptyBlock);
}
