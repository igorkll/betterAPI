#include <dsbox_net/body.hpp>

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace dsbox_net
{
    namespace
    {
        // A plain in-memory IByteStream reading from a shared, immutable
        // buffer -- the stream half of memorySource()'s IBodySource.
        // Several of these may exist at once over the same buffer (each
        // IBodySource::open() call makes a new one), which is exactly
        // why the buffer is const/shared rather than owned per-stream.
        class MemoryByteStream final : public IByteStream
        {
        public:
            explicit MemoryByteStream(std::shared_ptr<const std::vector<std::byte>> data)
                : m_data(std::move(data))
            {
            }

            std::size_t read(std::span<std::byte> out) override
            {
                if (m_closed || m_pos >= m_data->size())
                    return 0;

                const std::size_t remaining = m_data->size() - m_pos;
                const std::size_t n = std::min(remaining, out.size());

                std::memcpy(out.data(), m_data->data() + m_pos, n);
                m_pos += n;

                return n;
            }

            void close() noexcept override
            {
                m_closed = true;
            }

        private:
            std::shared_ptr<const std::vector<std::byte>> m_data;
            std::size_t m_pos = 0;
            bool m_closed = false;
        };

        class MemoryBodySource final : public IBodySource
        {
        public:
            explicit MemoryBodySource(std::vector<std::byte> bytes)
                : m_data(std::make_shared<std::vector<std::byte>>(std::move(bytes)))
            {
            }

            std::optional<std::uint64_t> size() const override
            {
                return static_cast<std::uint64_t>(m_data->size());
            }

            std::unique_ptr<IByteStream> open() override
            {
                return std::make_unique<MemoryByteStream>(m_data);
            }

        private:
            std::shared_ptr<std::vector<std::byte>> m_data;
        };
    }

    std::shared_ptr<IBodySource> memorySource(std::vector<std::byte> bytes)
    {
        return std::make_shared<MemoryBodySource>(std::move(bytes));
    }

    std::vector<std::byte> readAll(IByteStream& stream, std::size_t limit)
    {
        std::vector<std::byte> result;
        std::byte chunk[64 * 1024];

        while (true)
        {
            const std::size_t n = stream.read(std::span<std::byte>(chunk, sizeof(chunk)));

            if (n == 0)
                break;

            if (result.size() + n > limit)
            {
                throw NetException(NetError{
                    NetErrorCode::Internal,
                    "readAll: body exceeded the given limit",
                    0
                });
            }

            result.insert(result.end(), chunk, chunk + n);
        }

        return result;
    }

    // Shared state behind one BodyPipe: a bounded FIFO of byte chunks,
    // plus the finished/failed/readerClosed flags that let read() and
    // write() agree on when to stop blocking. One condition_variable is
    // enough (rather than one per direction) since the state that both
    // sides check to decide whether to keep waiting is guarded by the
    // same mutex and changes together; a spurious extra wakeup on the
    // "wrong" side just re-checks its own predicate and goes back to
    // sleep if nothing relevant changed.
    struct BodyPipeState
    {
        explicit BodyPipeState(std::size_t bodyPipeCapacity) : capacity(bodyPipeCapacity) {}

        std::mutex mutex;
        std::condition_variable cv;

        std::deque<std::vector<std::byte>> chunks;
        std::size_t frontOffset = 0; // bytes already consumed from chunks.front()
        std::size_t bufferedBytes = 0;
        std::size_t capacity; // 0 = unbounded

        bool finished = false;
        bool failed = false;
        NetError error;

        bool readerClosed = false;
    };

    namespace
    {
        class BodyPipeReader final : public IByteStream
        {
        public:
            explicit BodyPipeReader(std::shared_ptr<BodyPipeState> state)
                : m_state(std::move(state))
            {
            }

            ~BodyPipeReader() override
            {
                close();
            }

            std::size_t read(std::span<std::byte> out) override
            {
                std::unique_lock<std::mutex> lock(m_state->mutex);

                // readerClosed is part of the wake-up condition: close()
                // (possibly from another thread) must unblock a read()
                // that is already waiting -- see IByteStream::close().
                m_state->cv.wait(lock, [this]
                {
                    return !m_state->chunks.empty()
                        || m_state->finished
                        || m_state->failed
                        || m_state->readerClosed;
                });

                if (m_state->readerClosed)
                    return 0; // closed: behaves like EOF

                if (m_state->chunks.empty())
                {
                    if (m_state->failed)
                        throw NetException(m_state->error);

                    return 0; // finished, nothing left buffered
                }

                auto& front = m_state->chunks.front();
                const std::size_t available = front.size() - m_state->frontOffset;
                const std::size_t n = std::min(available, out.size());

                std::memcpy(out.data(), front.data() + m_state->frontOffset, n);

                m_state->frontOffset += n;
                m_state->bufferedBytes -= n;

                if (m_state->frontOffset == front.size())
                {
                    m_state->chunks.pop_front();
                    m_state->frontOffset = 0;
                }

                lock.unlock();
                m_state->cv.notify_all(); // wake a writer waiting on space

                return n;
            }

            void close() noexcept override
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);

                if (m_state->readerClosed)
                    return;

                m_state->readerClosed = true;
                m_state->chunks.clear();
                m_state->bufferedBytes = 0;

                m_state->cv.notify_all();
            }

        private:
            std::shared_ptr<BodyPipeState> m_state;
        };
    }

    bool BodyPipe::Writer::write(std::span<const std::byte> bytes)
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);

        if (m_state->finished || m_state->failed || m_state->readerClosed)
            return false;

        if (m_state->capacity > 0)
        {
            m_state->cv.wait(lock, [this]
            {
                return m_state->bufferedBytes < m_state->capacity
                    || m_state->readerClosed
                    || m_state->finished
                    || m_state->failed;
            });
        }

        if (m_state->readerClosed || m_state->finished || m_state->failed)
            return false;

        m_state->chunks.emplace_back(bytes.begin(), bytes.end());
        m_state->bufferedBytes += bytes.size();

        lock.unlock();
        m_state->cv.notify_all();

        return true;
    }

    void BodyPipe::Writer::finish()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);

        if (m_state->finished || m_state->failed)
            return;

        m_state->finished = true;
        m_state->cv.notify_all();
    }

    void BodyPipe::Writer::fail(NetError error)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);

        if (m_state->finished || m_state->failed)
            return;

        m_state->failed = true;
        m_state->error = std::move(error);
        m_state->cv.notify_all();
    }

    BodyPipe BodyPipe::create(std::size_t capacity)
    {
        BodyPipe pipe;
        pipe.m_state = std::make_shared<BodyPipeState>(capacity);
        return pipe;
    }

    std::unique_ptr<IByteStream> BodyPipe::reader() const
    {
        return std::make_unique<BodyPipeReader>(m_state);
    }
}
