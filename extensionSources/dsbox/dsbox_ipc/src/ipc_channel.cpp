#include <dsbox_ipc/ipc_channel.hpp>

#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace dsbox_ipc
{
    namespace detail
    {
        struct PendingEntry
        {
            ResponseCallback callback;
            dsbox_scheduler::TimerHandle timer; // empty if this request has no timeout
        };

        struct PendingTable
        {
            std::mutex mutex;
            std::unordered_map<std::uint64_t, PendingEntry> entries;
        };

        // The timeout of request `id` has elapsed: fail it, unless a
        // reply (or close()) got there first.
        void expire(PendingTable& table, std::uint64_t id)
        {
            ResponseCallback callback;

            {
                std::lock_guard<std::mutex> lock(table.mutex);

                const auto it = table.entries.find(id);
                if (it == table.entries.end())
                    return;

                callback = std::move(it->second.callback);
                table.entries.erase(it);
            }

            // Outside the lock: a callback is arbitrary user code (it
            // may even call back into this channel).
            if (callback)
                callback(false, {});
        }
    }

    namespace
    {
        std::vector<std::byte> toBytes(std::string_view text)
        {
            std::vector<std::byte> bytes(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
            return bytes;
        }
    }

    IpcChannel::IpcChannel(std::unique_ptr<IFrameCodec> codec, WriteFn write, IpcChannelOptions options)
        : m_codec(std::move(codec))
        , m_write(std::move(write))
        , m_options(std::move(options))
        , m_pending(std::make_shared<detail::PendingTable>())
    {
        if (!m_codec)
            throw std::invalid_argument("dsbox_ipc: IpcChannel requires a non-null codec");
        if (!m_write)
            throw std::invalid_argument("dsbox_ipc: IpcChannel requires a non-null write function");
    }

    IpcChannel::~IpcChannel()
    {
        close();
    }

    void IpcChannel::send(const Frame& frame)
    {
        m_write(m_codec->encode(frame));
    }

    void IpcChannel::onBytesReceived(std::span<const std::byte> bytes)
    {
        if (m_closed.load())
            return;

        for (Frame& frame : m_codec->feed(bytes))
            handleFrame(std::move(frame));
    }

    void IpcChannel::handleFrame(Frame frame)
    {
        switch (frame.kind)
        {
        case FrameKind::Response:
        case FrameKind::Error:
        {
            ResponseCallback callback;
            dsbox_scheduler::TimerHandle timer;
            {
                std::lock_guard<std::mutex> lock(m_pending->mutex);
                auto it = m_pending->entries.find(frame.id);
                if (it == m_pending->entries.end())
                    return; // no one waiting (already timed out/closed, or stray reply) -- drop it
                callback = std::move(it->second.callback);
                timer = std::move(it->second.timer);
                m_pending->entries.erase(it);
            }

            // The reply won: the timeout is no longer needed. (If it is
            // firing right now it lost the race for the entry above and
            // finds nothing to fail.)
            timer.cancel();

            callback(frame.kind == FrameKind::Response, std::move(frame.payload));
            return;
        }

        case FrameKind::Request:
        {
            RequestHandler handler;
            {
                std::lock_guard<std::mutex> lock(m_handlersMutex);
                auto it = m_handlers.find(frame.target);
                if (it != m_handlers.end())
                    handler = it->second;
            }

            if (!handler)
            {
                Frame error;
                error.kind = FrameKind::Error;
                error.id = frame.id;
                error.payload = toBytes("no handler registered for target: " + frame.target);
                send(error);
                return;
            }

            try
            {
                Frame response;
                response.kind = FrameKind::Response;
                response.id = frame.id;
                response.payload = handler(std::move(frame.payload));
                send(response);
            }
            catch (const std::exception& exception)
            {
                Frame error;
                error.kind = FrameKind::Error;
                error.id = frame.id;
                error.payload = toBytes(exception.what());
                send(error);
            }
            return;
        }

        case FrameKind::Notify:
        {
            RequestHandler handler;
            {
                std::lock_guard<std::mutex> lock(m_handlersMutex);
                auto it = m_handlers.find(frame.target);
                if (it != m_handlers.end())
                    handler = it->second;
            }

            if (handler)
            {
                try { handler(std::move(frame.payload)); }
                catch (...) { /* no reply channel for Notify -- nothing to report to */ }
            }
            return;
        }
        }
    }

    void IpcChannel::notify(std::string target, std::vector<std::byte> payload)
    {
        Frame frame;
        frame.kind = FrameKind::Notify;
        frame.target = std::move(target);
        frame.payload = std::move(payload);
        send(frame);
    }

    std::chrono::milliseconds IpcChannel::effectiveTimeout(
        std::optional<std::chrono::milliseconds> timeoutOverride) const
    {
        const std::chrono::milliseconds effective = timeoutOverride.value_or(m_options.defaultTimeout);

        // "No timeout", whether that came from the override or the
        // channel default.
        return effective > std::chrono::milliseconds::zero() ? effective : std::chrono::milliseconds::zero();
    }

    std::shared_ptr<dsbox_scheduler::Scheduler> IpcChannel::privateScheduler()
    {
        std::lock_guard<std::mutex> lock(m_privateSchedulerMutex);

        if (!m_privateScheduler)
        {
            // Timeouts are tiny and the callback they run is the same
            // user callback the old watcher thread ran, on that same
            // kind of dedicated thread: so no worker pool, just run it
            // on the timer thread itself.
            dsbox_scheduler::SchedulerOptions options;
            options.defaultExecutor = std::make_shared<dsbox_scheduler::InlineExecutor>();
            m_privateScheduler = std::make_shared<dsbox_scheduler::Scheduler>(std::move(options));
        }

        return m_privateScheduler;
    }

    dsbox_scheduler::TimerHandle IpcChannel::armTimeout(std::uint64_t id, std::chrono::milliseconds timeout)
    {
        // Weak: the timer must not keep the table (and through it, the
        // callbacks of requests nobody is waiting for any more) alive,
        // and a timer outliving this channel must find it gone.
        const std::weak_ptr<detail::PendingTable> weak = m_pending;

        dsbox_scheduler::Callback task = [weak, id]
        {
            if (const auto table = weak.lock())
                detail::expire(*table, id);
        };

        if (m_options.scheduler)
        {
            auto timer = m_options.scheduler->after(timeout, task);

            if (timer)
                return timer;

            // A stopped shared scheduler (application shutting down)
            // hands back an empty handle; fall through to the private one
            // rather than leaving this request without its timeout.
        }

        return privateScheduler()->after(timeout, std::move(task));
    }

    std::uint64_t IpcChannel::requestAsync(
        std::string target,
        std::vector<std::byte> payload,
        ResponseCallback onDone,
        std::optional<std::chrono::milliseconds> timeout)
    {
        const std::uint64_t id = m_nextId.fetch_add(1);

        if (m_closed.load())
        {
            if (onDone)
                onDone(false, {});
            return id;
        }

        const std::chrono::milliseconds timeoutMs = effectiveTimeout(timeout);

        bool closedMeanwhile = false;

        {
            std::lock_guard<std::mutex> lock(m_pending->mutex);

            // Re-checked under the table lock: close() sets m_closed
            // BEFORE draining the table under this same lock, so either
            // close() drains this entry, or we see the flag here and fail
            // it ourselves -- it can never be left pending forever.
            if (m_closed.load())
                closedMeanwhile = true;
            else
                m_pending->entries.emplace(id, detail::PendingEntry{ std::move(onDone), {} });
        }

        if (closedMeanwhile)
        {
            if (onDone)
                onDone(false, {});
            return id;
        }

        if (timeoutMs > std::chrono::milliseconds::zero())
        {
            dsbox_scheduler::TimerHandle timer = armTimeout(id, timeoutMs);

            std::lock_guard<std::mutex> lock(m_pending->mutex);

            const auto it = m_pending->entries.find(id);
            if (it != m_pending->entries.end())
                it->second.timer = std::move(timer);
            else
                timer.cancel(); // already answered, expired, or closed while we were arming it
        }

        Frame frame;
        frame.kind = FrameKind::Request;
        frame.id = id;
        frame.target = std::move(target);
        frame.payload = std::move(payload);
        send(frame);

        return id;
    }

    std::future<std::vector<std::byte>> IpcChannel::request(
        std::string target,
        std::vector<std::byte> payload,
        std::optional<std::chrono::milliseconds> timeout)
    {
        auto promise = std::make_shared<std::promise<std::vector<std::byte>>>();
        std::future<std::vector<std::byte>> future = promise->get_future();

        requestAsync(std::move(target), std::move(payload), [promise](bool ok, std::vector<std::byte> responsePayload)
        {
            if (ok)
                promise->set_value(std::move(responsePayload));
            else
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error("dsbox_ipc: request failed, timed out, or channel closed")));
        }, timeout);

        return future;
    }

    void IpcChannel::registerHandler(std::string target, RequestHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_handlersMutex);
        const auto [it, inserted] = m_handlers.emplace(std::move(target), std::move(handler));
        if (!inserted)
            throw std::runtime_error("dsbox_ipc: a handler is already registered for target '" + it->first + "'");
    }

    void IpcChannel::close()
    {
        if (m_closed.exchange(true))
            return; // already closed

        std::unordered_map<std::uint64_t, detail::PendingEntry> pending;
        {
            std::lock_guard<std::mutex> lock(m_pending->mutex);
            pending.swap(m_pending->entries);
        }

        // These requests are failed right below; their timeouts must not
        // fire afterwards (and would find nothing to fail anyway).
        for (auto& [id, entry] : pending)
            entry.timer.cancel();

        std::shared_ptr<dsbox_scheduler::Scheduler> own;
        {
            std::lock_guard<std::mutex> lock(m_privateSchedulerMutex);
            own = m_privateScheduler;
        }

        // Joins the private timer thread (a no-op if close() itself is
        // running on it, from a timeout callback -- see
        // Scheduler::stop()).
        if (own)
            own->stop();

        for (auto& [id, entry] : pending)
        {
            if (entry.callback)
                entry.callback(false, {});
        }
    }
}
