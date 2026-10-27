#include "test_framework.hpp"

#include <dsbox_net_file/file_protocol_handler.hpp>

#include <dsbox_net/body.hpp>

#include <dsbox/communication_manager.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

using namespace dsbox_net_file_tests;
using namespace dsbox_net_file;
using namespace dsbox_net;

namespace
{
    // Writes `content` to a fresh temp file and returns the file://
    // Request that should read it back, along with the path so the
    // caller can clean up. Uses FilePathStyle::Both so this works
    // whichever native path shape std::filesystem::temp_directory_path()
    // happens to produce on the machine actually running the test (a
    // POSIX "/tmp/..." or a Windows "C:\Users\...\Temp\...").
    std::filesystem::path makeTempFileWithContent(const std::string& content)
    {
        std::random_device rd;
        auto path = std::filesystem::temp_directory_path()
            / ("dsbox_net_file_test_" + std::to_string(rd()) + ".tmp");

        std::ofstream out(path, std::ios::binary);
        out << content;
        out.close();

        return path;
    }

    Request requestForNativePath(const std::filesystem::path& nativePath)
    {
        std::string generic = nativePath.generic_string();

        Request request;
        request.url.scheme = "file";
        request.url.path = generic.empty() || generic.front() != '/' ? "/" + generic : generic;

        return request;
    }

    FetchResult startSynchronously(dsbox_net::IProtocolHandler& handler, Request request)
    {
        FetchResult captured;
        bool called = false;

        handler.start(std::move(request), [&](FetchResult result)
        {
            captured = std::move(result);
            called = true;
        });

        DSBOX_NET_FILE_CHECK(called); // this module's start() always calls `done` synchronously
        return captured;
    }

    void testDefaultFallbackReadsRealFile()
    {
        auto path = makeTempFileWithContent("hello from disk");

        dsbox::CommunicationManager comm; // no override registered -- exercises the real fallback
        FileProtocolHandler handler(comm, "test.file.open.unused", FilePathStyle::Both);

        auto result = startSynchronously(handler, requestForNativePath(path));

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::Success);
        DSBOX_NET_FILE_CHECK(result.response.body != nullptr);

        auto bytes = readAll(*result.response.body, 1 << 20);
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        DSBOX_NET_FILE_CHECK_EQ(text, "hello from disk");

        std::filesystem::remove(path);
    }

    void testDefaultFallbackReportsNotFoundForMissingFile()
    {
        auto missing = std::filesystem::temp_directory_path() / "dsbox_net_file_test_missing.tmp";
        std::filesystem::remove(missing); // make sure it really doesn't exist

        dsbox::CommunicationManager comm;
        FileProtocolHandler handler(comm, "test.file.open.unused", FilePathStyle::Both);

        auto result = startSynchronously(handler, requestForNativePath(missing));

        DSBOX_NET_FILE_CHECK(result.ok()); // NotFound is a delivered Response, not a NetError
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::NotFound);
        DSBOX_NET_FILE_CHECK(result.response.body == nullptr);
    }

    void testOverrideReplacesDefaultFilesystemAccess()
    {
        dsbox::CommunicationManager comm;
        int overrideCalls = 0;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [&](std::string path) -> FileOpenResult
            {
                ++overrideCalls;

                FileOpenResult result;
                auto pipe = BodyPipe::create();
                auto writer = pipe.writer();

                const std::string body = "fake:" + path;
                auto bytes = std::vector<std::byte>(
                    reinterpret_cast<const std::byte*>(body.data()),
                    reinterpret_cast<const std::byte*>(body.data()) + body.size());

                writer.write(bytes);
                writer.finish();

                result.stream = pipe.reader();
                result.size = body.size();
                return result;
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::PosixOnly);

        Request request;
        request.url.scheme = "file";
        request.url.path = "/anything/at/all.txt";

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::Success);
        DSBOX_NET_FILE_CHECK_EQ(overrideCalls, 1);

        auto bytes = readAll(*result.response.body, 1024);
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        DSBOX_NET_FILE_CHECK_EQ(text, "fake:/anything/at/all.txt");
    }

    void testOverrideNotCalledWhenPathStyleRejectsUrl()
    {
        dsbox::CommunicationManager comm;
        int overrideCalls = 0;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [&](std::string) -> FileOpenResult
            {
                ++overrideCalls;
                return FileOpenResult{};
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::WindowsOnly);

        Request request;
        request.url.scheme = "file";
        request.url.path = "/home/user/file.txt"; // not a drive path -- WindowsOnly must reject it

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::NotFound);
        DSBOX_NET_FILE_CHECK_EQ(overrideCalls, 0);
    }

    void testDeniedFailureMapsToStatusClassDenied()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [](std::string) -> FileOpenResult
            {
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::Denied;
                result.message = "no permission";
                return result;
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::PosixOnly);

        Request request;
        request.url.scheme = "file";
        request.url.path = "/root/secret";

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::Denied);
        DSBOX_NET_FILE_CHECK_EQ(result.response.statusText, "no permission");
    }

    void testOtherFailureMapsToNetError()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [](std::string) -> FileOpenResult
            {
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::Other;
                result.message = "it's a directory";
                return result;
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::PosixOnly);

        Request request;
        request.url.scheme = "file";
        request.url.path = "/some/dir";

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(!result.ok());
        DSBOX_NET_FILE_CHECK(result.error->code == NetErrorCode::Internal);
    }

    void testUnregisteringOverrideRevertsToDefaultFallback()
    {
        auto path = makeTempFileWithContent("back to disk");

        dsbox::CommunicationManager comm;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [](std::string) -> FileOpenResult
            {
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::Other;
                result.message = "should not be reached after unregister";
                return result;
            });

        comm.unregisterHandler<FileOpenSignature>("test.file.open");

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::Both);
        auto result = startSynchronously(handler, requestForNativePath(path));

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::Success);

        std::filesystem::remove(path);
    }

    void testConstructorRejectsDefaultPathStyle()
    {
        // FileProtocolHandler has no configuration to resolve
        // FilePathStyle::Default against -- FileModule::onInit() is the
        // only place that resolution happens (see both classes' own
        // docs). Passing Default straight to the constructor is a
        // programming error, not a runtime condition, so it throws
        // rather than silently falling back to something.
        dsbox::CommunicationManager comm;
        bool threw = false;

        try
        {
            FileProtocolHandler handler(comm, "test.file.open.unused", FilePathStyle::Default);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }

        DSBOX_NET_FILE_CHECK(threw);
    }

    void testRemoteHostIsRefused()
    {
        dsbox::CommunicationManager comm;
        int overrideCalls = 0;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [&](std::string) -> FileOpenResult
            {
                ++overrideCalls;
                return FileOpenResult{};
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::PosixOnly);

        Request request;
        request.url.scheme = "file";
        request.url.host = "evil.example";
        request.url.path = "/etc/passwd";

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(!result.ok());
        DSBOX_NET_FILE_CHECK(result.error->code == NetErrorCode::UnsupportedOperation);
        DSBOX_NET_FILE_CHECK_EQ(overrideCalls, 0);
    }

    void testLocalhostHostIsAccepted()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<FileOpenSignature>(
            "test.file.open",
            [](std::string) -> FileOpenResult
            {
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::NotFound;
                return result;
            });

        FileProtocolHandler handler(comm, "test.file.open", FilePathStyle::PosixOnly);

        Request request;
        request.url.scheme = "file";
        request.url.host = "localhost";
        request.url.path = "/x";

        auto result = startSynchronously(handler, request);

        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::NotFound);
    }

    void testEncodedNulInPathIsRejected()
    {
        DSBOX_NET_FILE_CHECK(!nativePathFromUrlPath("/etc/passwd%00.txt", FilePathStyle::PosixOnly).has_value());
    }
}

void runFileProtocolHandlerTests()
{
    runTest("testDefaultFallbackReadsRealFile", testDefaultFallbackReadsRealFile);
    runTest("testDefaultFallbackReportsNotFoundForMissingFile", testDefaultFallbackReportsNotFoundForMissingFile);
    runTest("testOverrideReplacesDefaultFilesystemAccess", testOverrideReplacesDefaultFilesystemAccess);
    runTest("testOverrideNotCalledWhenPathStyleRejectsUrl", testOverrideNotCalledWhenPathStyleRejectsUrl);
    runTest("testDeniedFailureMapsToStatusClassDenied", testDeniedFailureMapsToStatusClassDenied);
    runTest("testOtherFailureMapsToNetError", testOtherFailureMapsToNetError);
    runTest("testUnregisteringOverrideRevertsToDefaultFallback", testUnregisteringOverrideRevertsToDefaultFallback);
    runTest("testConstructorRejectsDefaultPathStyle", testConstructorRejectsDefaultPathStyle);
    runTest("testRemoteHostIsRefused", testRemoteHostIsRefused);
    runTest("testLocalhostHostIsAccepted", testLocalhostHostIsAccepted);
    runTest("testEncodedNulInPathIsRejected", testEncodedNulInPathIsRejected);
}
