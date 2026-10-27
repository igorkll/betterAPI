#pragma once

#include <dsbox_ipc/api.hpp>
#include <dsbox_ipc/frame.hpp>
#include <dsbox_ipc/frame_codec.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace dsbox_scheduler
{
    class Scheduler;
    class TimerHandle;
}

namespace dsbox_ipc
{
    namespace detail
    {
        struct PendingTable; // defined in ipc_channel.cpp
    }

    using WriteFn = std::function<void(std::span<const std::byte>)>;
    using RequestHandler = std::function<std::vector<std::byte>(std::vector<std::byte>)>;
    using ResponseCallback = std::function<void(bool ok, std::vector<std::byte> payload)>;

    // Configuration for a single IpcChannel instance, fixed for its
    // whole lifetime.
    struct IpcChannelOptions
    {
        // Default time a request()/requestAsync() call is allowed to
        // wait for a matching Response/Error frame before it is failed
        // with ok=false, as if the channel had closed. 0 (the default)
        // means "wait indefinitely" -- matches this class's original
        // behavior. Override per call via request()/requestAsync()'s own
        // `timeout` parameter (which also accepts 0 to mean "no timeout
        // for this call", regardless of this default). A slow peer that
        // never replies would otherwise leave a caller's future/callback
        // pending forever; a bounded default guards against that.
        std::chrono::milliseconds defaultTimeout{ 0 };

        // Scheduler that times out requests. Optional: with null (the
        // default) the channel creates a small private one the first
        // time a request actually uses a timeout -- a single timer
        // thread, no worker pool -- and stops it in close(). Pass a
        // shared scheduler (e.g. the one SchedulerModule publishes) to
        // have every channel share one timer thread instead of each
        // starting its own.
        //
        // Where the failure callback of a timed-out request runs depends
        // on the scheduler: on the private one, on its timer thread; on a
        // shared one, on that scheduler's default executor. Either way
        // the existing rule holds -- it must not block.
        //
        // If a shared scheduler has been stopped (the application is
        // shutting down) the channel falls back to its private one, so a
        // timeout is never silently lost.
        std::shared_ptr<dsbox_scheduler::Scheduler> scheduler;
    };

    // The correlation-and-addressing layer of DSBox's IPC, sitting on
    // top of any raw byte-in/byte-out wire. IpcChannel has no idea where
    // the bytes actually come from or go to -- that is a WriteFn plus
    // whoever calls onBytesReceived() -- so the same class serves a
    // subprocess pipe, this process's own stdin/stdout, or (later) a
    // socket, without modification.
    //
    // Thread safety: onBytesReceived() must be called from only one
    // thread at a time (typically a single dedicated reader thread) --
    // the codec it owns is stateful and not reentrant. request(),
    // requestAsync(), notify(), and registerHandler() are all safe to
    // call concurrently from any thread, including from inside a
    // RequestHandler or ResponseCallback.
    class DSBOX_IPC_API IpcChannel : public std::enable_shared_from_this<IpcChannel>
    {
    public:
        IpcChannel(std::unique_ptr<IFrameCodec> codec, WriteFn write, IpcChannelOptions options = {});
        ~IpcChannel();

        IpcChannel(const IpcChannel&) = delete;
        IpcChannel& operator=(const IpcChannel&) = delete;

        // Feed bytes read from the wire. See the class-level note: only
        // one thread may call this at a time for a given instance.
        void onBytesReceived(std::span<const std::byte> bytes);

        // Fire-and-forget send; no reply is expected or waited for.
        void notify(std::string target, std::vector<std::byte> payload);

        // The request primitive. onDone is invoked exactly once: when a
        // Response/Error frame with the matching id arrives (from
        // whatever thread is pumping onBytesReceived), immediately with
        // ok=false if the channel is already closed, or with ok=false if
        // no reply arrives before `timeout` elapses (a timer on the
        // channel's scheduler, see IpcChannelOptions::scheduler, fires
        // it; the timer is cancelled as soon as the reply arrives).
        // Because it can run on the reader thread,
        // onDone must not block or it will stall delivery of every other
        // in-flight frame.
        //
        // `timeout`: std::nullopt (the default) uses
        // IpcChannelOptions::defaultTimeout; an explicit value overrides
        // it for this call only, and 0 explicitly means "no timeout"
        // regardless of the channel's default.
        //
        // Returns the request's correlation id (informational only).
        std::uint64_t requestAsync(
            std::string target,
            std::vector<std::byte> payload,
            ResponseCallback onDone,
            std::optional<std::chrono::milliseconds> timeout = std::nullopt);

        // Convenience wrapper over requestAsync() for callers who'd
        // rather block/co_await than write a callback. The future is
        // satisfied from whatever thread pumps onBytesReceived (or the
        // timeout watcher thread, if `timeout` elapses first), so avoid
        // calling get() from that same thread (it would deadlock). See
        // requestAsync() for what `timeout` means.
        std::future<std::vector<std::byte>> request(
            std::string target,
            std::vector<std::byte> payload,
            std::optional<std::chrono::milliseconds> timeout = std::nullopt);

        // At most one handler per target. Throws std::runtime_error if a
        // handler is already registered for this target (mirrors
        // CommunicationManager's one-handler-per-name semantics, which
        // this deliberately follows since both express "who owns
        // target X" for the same kind of addressing problem).
        void registerHandler(std::string target, RequestHandler handler);

        // Fails every still-pending request with ok=false and stops
        // delivering incoming frames, so callers blocked on request()'s
        // future (or waiting on a requestAsync() callback) are not left
        // hanging when the underlying wire goes away. Idempotent, safe
        // to call from any thread including a handler.
        void close();

    private:
        void handleFrame(Frame frame);
        void send(const Frame& frame);

        // 0 means "no timeout" (whether from the override or the default).
        std::chrono::milliseconds effectiveTimeout(
            std::optional<std::chrono::milliseconds> timeoutOverride) const;

        // Arms a one-shot timer that fails request `id` after `timeout`.
        // Returns an empty handle only if no scheduler at all would take
        // it, which cannot happen (see IpcChannelOptions::scheduler).
        dsbox_scheduler::TimerHandle armTimeout(std::uint64_t id, std::chrono::milliseconds timeout);

        std::shared_ptr<dsbox_scheduler::Scheduler> privateScheduler();

        std::unique_ptr<IFrameCodec> m_codec;
        WriteFn m_write;
        IpcChannelOptions m_options;

        std::atomic<std::uint64_t> m_nextId{ 1 };
        std::atomic<bool> m_closed{ false };

        // Requests awaiting a reply, with the timer (if any) that will
        // fail each. Held through a shared_ptr, and referenced from the
        // timers only weakly: a timer that fires while this channel is
        // being destroyed finds the table gone and does nothing, so no
        // timer callback can ever touch a dead IpcChannel -- without
        // having to wait for in-flight timers in close().
        std::shared_ptr<detail::PendingTable> m_pending;

        // Created on first use if IpcChannelOptions::scheduler is null
        // (or as the fallback when that one is stopped); stopped in
        // close().
        std::mutex m_privateSchedulerMutex;
        std::shared_ptr<dsbox_scheduler::Scheduler> m_privateScheduler;

        std::mutex m_handlersMutex;
        std::unordered_map<std::string, RequestHandler> m_handlers;
    };
}
