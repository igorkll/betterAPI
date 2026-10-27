#include "test_framework.hpp"

#include <dsbox_process/subprocess.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dsbox_process_tests;

namespace
{
    std::vector<std::byte> toBytes(const std::string& text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    // "sort" exists as a first-party executable on both Windows
    // (System32\sort.exe) and POSIX (coreutils) and is resolved via PATH
    // on both platforms (see CreateProcessW/posix_spawnp usage), so it
    // makes a reasonable cross-platform smoke-test target: reads all of
    // stdin, sorts lines, writes them to stdout, exits 0 on stdin EOF.
    void testRoundTripThroughSort()
    {
        dsbox_process::SubprocessOptions options;
        options.executable = "sort";

        dsbox_process::Subprocess proc(options);

        std::string collected;
        std::mutex collectedMutex;
        proc.onStdoutData([&](std::span<const std::byte> data)
        {
            std::lock_guard<std::mutex> lock(collectedMutex);
            collected.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        std::atomic<int> exitCode{ -1 };
        proc.onExit([&](int code) { exitCode = code; });

        proc.writeStdin(toBytes("banana\r\napple\r\n"));
        proc.closeStdin();

        const int code = proc.wait();

        DSBOX_PROCESS_CHECK_EQ(code, 0);
        DSBOX_PROCESS_CHECK_EQ(exitCode.load(), 0);
        DSBOX_PROCESS_CHECK(!proc.isRunning());

        std::lock_guard<std::mutex> lock(collectedMutex);
        DSBOX_PROCESS_CHECK(collected.find("apple") != std::string::npos);
        DSBOX_PROCESS_CHECK(collected.find("banana") != std::string::npos);
        DSBOX_PROCESS_CHECK(collected.find("apple") < collected.find("banana"));
    }

    void testThrowsOnMissingExecutable()
    {
        dsbox_process::SubprocessOptions options;
        options.executable = "dsbox_definitely_not_a_real_executable_xyz";

        bool threw = false;
        try
        {
            dsbox_process::Subprocess proc(options);
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_PROCESS_CHECK(threw);
    }
}

void runSubprocessTests()
{
    runTest("RoundTripThroughSort", testRoundTripThroughSort);
    runTest("ThrowsOnMissingExecutable", testThrowsOnMissingExecutable);
}
