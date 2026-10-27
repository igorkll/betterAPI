#include "test_framework.hpp"

#include <dsbox_net/body.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace dsbox_net_tests;
using namespace dsbox_net;

namespace
{
    std::vector<std::byte> toBytes(const char* text)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(text);
        return std::vector<std::byte>(begin, begin + std::strlen(text));
    }

    std::string toText(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    void testMemorySourceReadAll()
    {
        auto source = memorySource(toBytes("hello world"));
        DSBOX_NET_CHECK(source->size().has_value());
        DSBOX_NET_CHECK_EQ(*source->size(), static_cast<std::uint64_t>(11));

        auto stream = source->open();
        auto bytes = readAll(*stream, 1024);
        DSBOX_NET_CHECK_EQ(toText(bytes), "hello world");
    }

    void testMemorySourceOpensRepeatedly()
    {
        auto source = memorySource(toBytes("abc"));

        auto first = readAll(*source->open(), 1024);
        auto second = readAll(*source->open(), 1024);

        DSBOX_NET_CHECK_EQ(toText(first), "abc");
        DSBOX_NET_CHECK_EQ(toText(second), "abc");
    }

    void testReadAllThrowsWhenLimitExceeded()
    {
        auto source = memorySource(toBytes("0123456789"));
        auto stream = source->open();

        bool threw = false;
        try
        {
            readAll(*stream, 5);
        }
        catch (const NetException&)
        {
            threw = true;
        }

        DSBOX_NET_CHECK(threw);
    }

    void testBodyPipeWriteThenReadThenFinish()
    {
        auto pipe = BodyPipe::create(1024);
        auto writer = pipe.writer();
        auto reader = pipe.reader();

        auto data = toBytes("chunk1");
        DSBOX_NET_CHECK(writer.write(data));
        writer.finish();

        auto result = readAll(*reader, 1024);
        DSBOX_NET_CHECK_EQ(toText(result), "chunk1");
    }

    void testBodyPipeFailPropagatesToReader()
    {
        auto pipe = BodyPipe::create(1024);
        auto writer = pipe.writer();
        auto reader = pipe.reader();

        writer.fail(NetError{ NetErrorCode::ConnectionReset, "peer reset", 0 });

        bool threw = false;
        try
        {
            readAll(*reader, 1024);
        }
        catch (const NetException& exception)
        {
            threw = true;
            DSBOX_NET_CHECK(exception.error().code == NetErrorCode::ConnectionReset);
        }

        DSBOX_NET_CHECK(threw);
    }

    void testBodyPipeBackpressureBlocksWriter()
    {
        auto pipe = BodyPipe::create(4); // tiny capacity
        auto writer = pipe.writer();
        auto reader = pipe.reader();

        // Fills the buffer without blocking.
        DSBOX_NET_CHECK(writer.write(toBytes("abcd")));

        std::atomic<bool> secondWriteReturned{ false };

        std::thread writerThread([&]
        {
            writer.write(toBytes("efgh")); // should block until we read below
            secondWriteReturned = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        DSBOX_NET_CHECK(!secondWriteReturned); // still blocked -- buffer was full

        std::byte drain[4];
        const auto n = reader->read(std::span<std::byte>(drain, 4));
        DSBOX_NET_CHECK_EQ(n, static_cast<std::size_t>(4)); // frees capacity for the writer

        writerThread.join();
        DSBOX_NET_CHECK(secondWriteReturned);

        writer.finish();
        auto rest = readAll(*reader, 1024);
        DSBOX_NET_CHECK_EQ(toText(rest), "efgh");
    }

    void testBodyPipeReaderCloseUnblocksWriter()
    {
        auto pipe = BodyPipe::create(4);
        auto writer = pipe.writer();
        auto reader = pipe.reader();

        DSBOX_NET_CHECK(writer.write(toBytes("abcd")));

        std::atomic<bool> wroteFalse{ false };

        std::thread writerThread([&]
        {
            const bool ok = writer.write(toBytes("more")); // blocked until close()
            wroteFalse = !ok;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        reader->close();

        writerThread.join();
        DSBOX_NET_CHECK(wroteFalse);
    }

    void testBodyPipeCloseUnblocksWaitingRead()
    {
        auto pipe = BodyPipe::create();
        auto reader = pipe.reader();

        std::atomic<bool> returned{ false };
        std::size_t got = 99;

        std::thread readerThread([&]
        {
            std::byte buffer[8];
            got = reader->read(std::span<std::byte>(buffer, sizeof(buffer))); // blocks: nothing written yet
            returned = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        DSBOX_NET_CHECK(!returned);

        reader->close();
        readerThread.join();

        DSBOX_NET_CHECK(returned);
        DSBOX_NET_CHECK_EQ(got, static_cast<std::size_t>(0));
    }
}

void runBodyTests()
{
    runTest("testMemorySourceReadAll", testMemorySourceReadAll);
    runTest("testMemorySourceOpensRepeatedly", testMemorySourceOpensRepeatedly);
    runTest("testReadAllThrowsWhenLimitExceeded", testReadAllThrowsWhenLimitExceeded);
    runTest("testBodyPipeWriteThenReadThenFinish", testBodyPipeWriteThenReadThenFinish);
    runTest("testBodyPipeFailPropagatesToReader", testBodyPipeFailPropagatesToReader);
    runTest("testBodyPipeBackpressureBlocksWriter", testBodyPipeBackpressureBlocksWriter);
    runTest("testBodyPipeReaderCloseUnblocksWriter", testBodyPipeReaderCloseUnblocksWriter);
    runTest("testBodyPipeCloseUnblocksWaitingRead", testBodyPipeCloseUnblocksWaitingRead);
}
