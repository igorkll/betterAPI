#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/error.hpp>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace dsbox_net
{
    // A response body, exposed as a blocking pull stream: read() blocks
    // the calling thread until at least one byte is available, EOF is
    // reached, or the transfer fails. This is the simplest model that
    // covers every protocol handler this library targets without
    // forcing a particular concurrency style on callers -- see the
    // design note in NetworkManager's own header for the trade-off
    // (one thread per active reader) this implies and when to prefer
    // it over a push/pause-resume model instead.
    class DSBOX_NET_API IByteStream
    {
    public:
        virtual ~IByteStream() = default;

        // Reads up to `out.size()` bytes into `out`. Returns the number
        // actually read; 0 means EOF (subsequent calls also return 0).
        // Throws NetException if the transfer fails before EOF -- e.g.
        // the peer resets the connection partway through the body.
        virtual std::size_t read(std::span<std::byte> out) = 0;

        // Releases any underlying resource (socket, file handle, ...)
        // and causes any in-progress or future read() to stop blocking:
        // a read() already waiting for more data returns whatever it
        // has (possibly 0, i.e. behaves like EOF) rather than hanging
        // forever. Safe to call from a different thread than the one
        // calling read(), and safe to call more than once. Does not
        // throw.
        virtual void close() noexcept = 0;
    };

    // A request body that can be (re-)opened into a byte stream. Kept
    // separate from IByteStream itself because a body may need to be
    // read more than once -- a 307/308 redirect that must resend the
    // same body, or a request retried after a transient connection
    // failure -- and "rewind" is not something every stream naturally
    // supports (a live upload from another process's stdout, say,
    // genuinely cannot be re-read). Rather than have every IByteStream
    // implementation answer "can you rewind", the source is asked to
    // open() a fresh stream each time instead.
    class DSBOX_NET_API IBodySource
    {
    public:
        virtual ~IBodySource() = default;

        // Total size in bytes if known ahead of time (lets a handler
        // send a Content-Length instead of chunked encoding); nullopt if
        // it can't be known without reading the whole thing.
        virtual std::optional<std::uint64_t> size() const = 0;

        // Opens a fresh read of this body from the beginning. May be
        // called more than once over this source's lifetime (see the
        // class comment on redirects/retries); a source that genuinely
        // cannot be re-read should throw NetException(UnsupportedOperation)
        // from the second call onward rather than silently returning an
        // empty stream.
        virtual std::unique_ptr<IByteStream> open() = 0;
    };

    // An IBodySource wrapping an in-memory buffer -- the common case
    // (small JSON/form bodies, ...): always knows its size, and open()
    // can be called any number of times since the bytes never move.
    DSBOX_NET_API std::shared_ptr<IBodySource> memorySource(std::vector<std::byte> bytes);

    // Reads `stream` to EOF and returns the accumulated bytes. Throws
    // NetException(Internal) if more than `limit` bytes are read before
    // EOF, to bound memory use for a caller that forgot the body could
    // be large -- pass a large `limit` explicitly if that's genuinely
    // intended. Propagates whatever NetException `stream.read()` itself
    // throws.
    DSBOX_NET_API std::vector<std::byte> readAll(IByteStream& stream, std::size_t limit);

    // A bounded producer/consumer bridge between "bytes arrive
    // asynchronously from the wire" (the producer side, typically a
    // protocol handler's own I/O thread) and "the caller pulls them via
    // IByteStream::read()" (the consumer side, handed out as the
    // Response's body). This is the one concrete IByteStream this
    // library provides; a protocol handler is free to implement its own
    // instead when that fits its I/O model better.
    //
    // Backpressure: Writer::write() blocks its caller once `capacity`
    // bytes are buffered and not yet read, until the reader catches up
    // -- so a slow consumer naturally throttles a fast producer instead
    // of buffering an unbounded amount of memory.
    class DSBOX_NET_API BodyPipe
    {
    public:
        // The producer-facing half. Copyable (cheap: shares the
        // underlying buffer), so a protocol handler can capture it by
        // value in whatever callback its I/O layer invokes.
        //
        // Needs its own DSBOX_NET_API: MSVC's dllexport on the enclosing
        // BodyPipe class does NOT propagate to a nested class's own
        // members -- without this, write()/finish()/fail() (defined
        // out-of-line in body.cpp) are compiled into the DLL but never
        // exported, and anything linking against DSBoxNet fails with
        // LNK2019 on exactly those three symbols.
        class DSBOX_NET_API Writer
        {
        public:
            // Appends `bytes`, blocking the caller while the buffer is
            // already at (or would exceed) capacity. Returns false
            // (without blocking further, and without appending) once the
            // reader side has been closed -- there is no point buffering
            // more data nobody will ever read.
            bool write(std::span<const std::byte> bytes);

            // Marks the stream complete: once every already-buffered
            // byte has been read, subsequent read() calls return 0
            // (EOF). Idempotent; a call after fail() has already been
            // called is a no-op (the failure stands).
            void finish();

            // Marks the stream as failed: read() throws NetException(error)
            // once it has delivered whatever was already buffered.
            // Idempotent, and wins over a finish() that hasn't happened
            // yet -- but a finish()/fail() that already ran is not
            // overwritten by a later call to either.
            void fail(NetError error);

        private:
            friend class BodyPipe;
            explicit Writer(std::shared_ptr<struct BodyPipeState> state) : m_state(std::move(state)) {}
            std::shared_ptr<struct BodyPipeState> m_state;
        };

        // Creates a linked writer/reader pair. `capacity` is the
        // buffering bound described above; 0 means unbounded (matches
        // IOModule's own convention for the same kind of knob).
        static BodyPipe create(std::size_t capacity = 256 * 1024);

        Writer writer() const
        {
            return Writer(m_state);
        }

        // The consumer-facing IByteStream. Ownership passes to the
        // caller (typically placed straight into Response::body); the
        // Writer above remains usable independently.
        std::unique_ptr<IByteStream> reader() const;

    private:
        std::shared_ptr<struct BodyPipeState> m_state;
    };
}
