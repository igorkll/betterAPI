#pragma once

#include <dsbox_io/api.hpp>

#include <dsbox/module.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dsbox_ipc
{
    class IFrameCodec;
    class IpcChannel;
}

namespace dsbox_process
{
    struct SubprocessOptions;
}

namespace dsbox_io
{
    // Published via CommunicationManager::emit<SubprocessExitEvent> on
    // "io.subprocess.exit" whenever a subprocess connected through
    // connectSubprocess() exits, for whatever reason (including
    // disconnectSubprocess() and IOModule shutdown forcibly terminating
    // it).
    struct SubprocessExitEvent
    {
        std::string name;
        int exitCode = 0;
    };

    // Configuration for a single IOModule instance, fixed for its whole
    // lifetime (no setters -- construct a new instance if you need
    // different behavior).
    struct IOOptions
    {
        // Maximum number of not-yet-delivered stdout messages write()
        // will let build up in the internal queue before it starts
        // blocking the calling thread until the worker thread catches
        // up. 0 (the default) means unbounded -- matches this class's
        // original behavior. Set this on a producer that could otherwise
        // outpace a slow or overridden "io.stdout.write" sink and grow
        // the queue without limit. Does not apply to writeError(), which
        // is always synchronous, or to connectAsChild()'s raw byte
        // stream, which has its own flow control via the IPC channel.
        std::size_t maxQueueSize = 0;

        // Size, in bytes, of the read buffer used by connectAsChild()'s
        // background thread when pumping this process's own stdin into
        // the child IPC channel. Larger values reduce the number of
        // onBytesReceived()/read() round trips for a high-throughput
        // link; smaller values reduce the worst-case latency between a
        // byte arriving on stdin and it reaching the channel. Must be
        // greater than 0.
        std::size_t childReadBufferSize = 64 * 1024;
    };

    // Manages the process's standard streams -- stdout, stderr, stdin --
    // AND every dsbox_ipc channel this process is a party to, whichever
    // direction it runs: this process talking to a subprocess it spawned
    // (connectSubprocess), or this process itself being the "child" on
    // someone else's IPC, addressed via its own stdin/stdout
    // (connectAsChild). Both are just dsbox_ipc::IpcChannel wired to a
    // byte source/sink -- this module owns the wiring (and, for
    // subprocesses, the dsbox_process::Subprocess underneath), it does
    // not reimplement dsbox_ipc's or dsbox_process's logic.
    //
    //   write(text)        -- stdout, for human-readable logs.
    //                          Asynchronous: queued, drained by a
    //                          dedicated worker thread (default
    //                          std::cout, or whatever "io.stdout.write"
    //                          override is registered). See the
    //                          IMPORTANT note below about how this
    //                          interacts with connectAsChild(), and
    //                          IOOptions::maxQueueSize for bounding how
    //                          far the queue can grow.
    //
    //   writeError(text)   -- stderr. Synchronous, immediate, via
    //                          "io.stderr.write" or std::cerr. Errors are
    //                          low-volume/high-criticality: a message
    //                          sitting in a queue that never drains again
    //                          is a message lost right when it mattered.
    //
    //   enableStdin()      -- stdin, LINE mode: a background thread
    //                          publishes each line read from stdin as an
    //                          "io.stdin.line" event. For a human typing
    //                          at a console. Mutually exclusive with
    //                          connectAsChild() -- stdin can only be read
    //                          one way; whichever of the two is called
    //                          first claims it, the other throws
    //                          std::runtime_error. If stdin is a real
    //                          interactive terminal, this hands it to a
    //                          replxx line editor instead of a bare
    //                          std::getline() loop, so write()/
    //                          writeError() can erase and redraw an
    //                          in-progress input line around a log
    //                          message instead of the message splitting
    //                          it apart on screen; piped/redirected
    //                          stdin falls back to plain std::getline(),
    //                          unchanged from before.
    //
    //   connectAsChild()   -- stdin+stdout, RAW BYTE mode: wires this
    //                          process's own stdin/stdout into a fresh
    //                          IpcChannel, for when this DSBox process is
    //                          itself a subprocess under someone else's
    //                          IPC. IMPORTANT: once this is active,
    //                          write() no longer touches stdout at all --
    //                          it silently redirects to writeError()
    //                          instead, because stdout is now the wire
    //                          and a stray human-readable log line
    //                          written into the middle of a frame would
    //                          corrupt it. Call this as early as possible
    //                          (ideally right after onInit()) so nothing
    //                          else gets a chance to write to stdout
    //                          first.
    //
    //   connectSubprocess()-- spawns a dsbox_process::Subprocess and
    //                          wires it into a fresh IpcChannel: the
    //                          child's raw stdout feeds the channel, the
    //                          channel's outgoing bytes go to the
    //                          child's stdin, and -- for free, since this
    //                          module already owns stderr -- the child's
    //                          stderr lines are forwarded through this
    //                          module's own writeError(), so a spawned
    //                          DSBox subprocess's logs show up exactly
    //                          like this process's own. Channels are
    //                          named and kept in a registry; look them up
    //                          again with subprocessChannel(), tear one
    //                          down explicitly with
    //                          disconnectSubprocess(), or let
    //                          onShutdown() close/terminate all of them.
    //
    // Request timeouts: every channel created by connectAsChild() and
    // connectSubprocess() runs its request timeouts (see
    // dsbox_ipc::IpcChannel::requestAsync()) on the shared
    // dsbox_scheduler::Scheduler published in the Context -- e.g. by
    // dsbox_scheduler::SchedulerModule -- so any number of channels share
    // one timer thread. If there is none when a channel is created, that
    // channel uses a private scheduler of its own, created the first time
    // one of its requests actually has a timeout. The scheduler is
    // optional: nothing here depends on it, and no dependency on
    // "dsbox.scheduler" needs declaring.
    //
    // Lifecycle: the stdout worker thread starts at the end of onInit()
    // and is stopped -- fully drained and joined -- in onShutdown(),
    // which also closes connectAsChild()'s channel (if any) and
    // disconnects every subprocess (closing its channel and letting
    // dsbox_process::Subprocess's own destructor forcibly terminate it if
    // it is still running -- see that class's docs). The destructor
    // performs the same teardown if onShutdown was never called, so an
    // IOModule is always safe to simply destroy.
    //
    // write()/writeError() calls before onInit() has run, or after
    // onShutdown() has already run, fall back to writing directly instead
    // of being lost. A misbehaving override for either stream cannot
    // bring down the process: exceptions are caught and that one message
    // is dropped, delivery continues with the next.
    class DSBOX_IO_API IOModule final : public dsbox::IModule
    {
    public:
        IOModule();
        explicit IOModule(IOOptions options);
        ~IOModule() override;

        IOModule(const IOModule&) = delete;
        IOModule& operator=(const IOModule&) = delete;

        std::string_view name() const override
        {
            return "dsbox.io";
        }

        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Safe to call concurrently from any number of threads. Blocks
        // the caller if IOOptions::maxQueueSize is set and the queue is
        // currently full, until the worker makes room or the module
        // starts shutting down.
        void write(std::string text);

        // Safe to call concurrently from any number of threads.
        void writeError(std::string text);

        // Starts a background thread that reads lines from std::cin and
        // emits each as an "io.stdin.line" event. Safe to call at most
        // once; a second call is a no-op. Throws std::runtime_error if
        // connectAsChild() has already claimed stdin. Does nothing if
        // called before onInit() (there is no Context yet to emit
        // through).
        //
        // Caveat -- read this before using: the reader thread is
        // detached, not joined, and keeps running until stdin reaches
        // EOF or the process exits; there is no portable way to
        // interrupt a thread blocked in std::getline(). Only call this
        // on an IOModule instance whose Context is guaranteed to live
        // for the remaining lifetime of the process -- see the same
        // caveat spelled out in more detail in the .cpp history/git log
        // for this method. Do not call this on a module you might
        // unload() or reload().
        void enableStdin();

        // Reads one line on the calling thread. Uses the same terminal
        // editor/output coordination as enableStdin(), but lets an
        // interactive command loop stop reading before module shutdown.
        // An aborted replxx edit on a still-connected terminal is returned
        // as an empty line; nullopt means stdin reached EOF.
        std::optional<std::string> readStdinLine();

        // Wires this process's own stdin/stdout into a fresh IpcChannel
        // for the "we are the child" scenario. codec defaults to
        // TextLineFrameCodec (the natural choice for a stream that may be
        // shared with a human or another line-oriented reader); pass a
        // different codec if the parent process speaks something else.
        // Throws std::runtime_error if enableStdin() already claimed
        // stdin, or if this has already been called once (at most one
        // child link per process). Returns the channel so the caller can
        // registerHandler()/request()/notify() on it immediately.
        std::shared_ptr<dsbox_ipc::IpcChannel> connectAsChild(
            std::unique_ptr<dsbox_ipc::IFrameCodec> codec = nullptr);

        // Spawns a subprocess and wires it into a fresh IpcChannel, kept
        // in this module's registry under `name`. codec defaults to
        // LengthPrefixedFrameCodec (a subprocess's stdout/stdin is a
        // dedicated pipe nobody else shares, so a binary-safe payload is
        // fine). Throws std::runtime_error if `name` is already in use,
        // or whatever dsbox_process::Subprocess's constructor throws if
        // spawning fails.
        std::shared_ptr<dsbox_ipc::IpcChannel> connectSubprocess(
            std::string name,
            dsbox_process::SubprocessOptions options,
            std::unique_ptr<dsbox_ipc::IFrameCodec> codec = nullptr);

        // Returns nullptr if no subprocess is registered under this name.
        std::shared_ptr<dsbox_ipc::IpcChannel> subprocessChannel(std::string_view name) const;

        // Closes the channel and destroys the Subprocess (forcibly
        // terminating it if it's still running -- see
        // dsbox_process::Subprocess's destructor docs). No-op if `name`
        // isn't registered.
        void disconnectSubprocess(std::string_view name);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
