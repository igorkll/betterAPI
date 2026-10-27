#include "test_framework.hpp"

#include <dsbox_net_file/file_protocol_handler.hpp>

using namespace dsbox_net_file_tests;
using namespace dsbox_net_file;

namespace
{
    void testPosixOnlyPassesPathThrough()
    {
        auto result = nativePathFromUrlPath("/home/user/file.txt", FilePathStyle::PosixOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "/home/user/file.txt");
    }

    void testPosixOnlyDoesNotSpecialCaseDriveLetters()
    {
        // Under PosixOnly, "/C:/Users/x" is just a literal (and, on a
        // real POSIX filesystem, nonexistent) path -- NOT converted.
        auto result = nativePathFromUrlPath("/C:/Users/x", FilePathStyle::PosixOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "/C:/Users/x");
    }

    void testWindowsOnlyConvertsDrivePath()
    {
        auto result = nativePathFromUrlPath("/C:/Users/x/file.txt", FilePathStyle::WindowsOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "C:\\Users\\x\\file.txt");
    }

    void testWindowsOnlyConvertsDriveRoot()
    {
        auto result = nativePathFromUrlPath("/C:", FilePathStyle::WindowsOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "C:\\");
    }

    void testWindowsOnlyUppercasesDriveLetter()
    {
        auto result = nativePathFromUrlPath("/c:/Users/x", FilePathStyle::WindowsOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "C:\\Users\\x");
    }

    void testWindowsOnlyRejectsNonDrivePath()
    {
        auto result = nativePathFromUrlPath("/home/user/file.txt", FilePathStyle::WindowsOnly);
        DSBOX_NET_FILE_CHECK(!result.has_value());
    }

    void testBothConvertsDrivePathsAndPassesOthersThrough()
    {
        auto windows = nativePathFromUrlPath("/D:/data/file.bin", FilePathStyle::Both);
        DSBOX_NET_FILE_CHECK(windows.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*windows, "D:\\data\\file.bin");

        auto posix = nativePathFromUrlPath("/home/user/file.txt", FilePathStyle::Both);
        DSBOX_NET_FILE_CHECK(posix.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*posix, "/home/user/file.txt");
    }

    void testPercentDecodingHappensBeforePathStyleLogic()
    {
        // "%20" -> space; also confirms decoding doesn't disturb the
        // drive-letter detection when it comes right after.
        auto result = nativePathFromUrlPath("/C%3A/Program%20Files/app", FilePathStyle::WindowsOnly);
        DSBOX_NET_FILE_CHECK(result.has_value());
        DSBOX_NET_FILE_CHECK_EQ(*result, "C:\\Program Files\\app");
    }

    void testRejectsPathNotStartingWithSlash()
    {
        // Shouldn't happen for a URL that came from dsbox_net::Url::parse
        // (file:// paths are always absolute), but nativePathFromUrlPath
        // is a standalone function and must not misbehave if called
        // directly with something malformed.
        auto result = nativePathFromUrlPath("relative/path", FilePathStyle::PosixOnly);
        DSBOX_NET_FILE_CHECK(!result.has_value());
    }

    void testDefaultStyleIsRejectedAsNotARealStyle()
    {
        // FilePathStyle::Default only means anything to
        // FileModule::onInit(), which resolves it before this function
        // (or FileProtocolHandler's constructor) ever sees it -- see
        // both classes' own docs.
        auto result = nativePathFromUrlPath("/home/user/file.txt", FilePathStyle::Default);
        DSBOX_NET_FILE_CHECK(!result.has_value());
    }
}

void runPathStyleTests()
{
    runTest("testPosixOnlyPassesPathThrough", testPosixOnlyPassesPathThrough);
    runTest("testPosixOnlyDoesNotSpecialCaseDriveLetters", testPosixOnlyDoesNotSpecialCaseDriveLetters);
    runTest("testWindowsOnlyConvertsDrivePath", testWindowsOnlyConvertsDrivePath);
    runTest("testWindowsOnlyConvertsDriveRoot", testWindowsOnlyConvertsDriveRoot);
    runTest("testWindowsOnlyUppercasesDriveLetter", testWindowsOnlyUppercasesDriveLetter);
    runTest("testWindowsOnlyRejectsNonDrivePath", testWindowsOnlyRejectsNonDrivePath);
    runTest("testBothConvertsDrivePathsAndPassesOthersThrough", testBothConvertsDrivePathsAndPassesOthersThrough);
    runTest("testPercentDecodingHappensBeforePathStyleLogic", testPercentDecodingHappensBeforePathStyleLogic);
    runTest("testRejectsPathNotStartingWithSlash", testRejectsPathNotStartingWithSlash);
    runTest("testDefaultStyleIsRejectedAsNotARealStyle", testDefaultStyleIsRejectedAsNotARealStyle);
}
