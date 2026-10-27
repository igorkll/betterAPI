#include <dsbox_io/io_module.hpp>

#include <dsbox/context.hpp>
#include <dsbox/overridable.hpp>
#include <dsbox/static_registry.hpp>

#include <dsbox_ipc/frame_codec.hpp>
#include <dsbox_ipc/ipc_channel.hpp>
#include <dsbox_process/subprocess.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <replxx.hxx>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <io.h>
    #define DSBOX_IO_ISATTY _isatty
    #define DSBOX_IO_FILENO _fileno
#else
    #include <unistd.h>
    #define DSBOX_IO_ISATTY isatty
    #define DSBOX_IO_FILENO fileno
#endif

namespace dsbox_io
{
    struct IOModule::Impl
    {
        explicit Impl(IOOptions opts) : options(std::move(opts)) {}

        struct SubprocessEntry
        {
            std::unique_ptr<dsbox_process::Subprocess> process;
            std::shared_ptr<dsbox_ipc::IpcChannel> channel;
        };

        IOOptions options;

        dsbox::Context* context = nullptr;

        std::optional<dsbox::Overridable<void(std::string)>> writeOverride;
        std::optional<dsbox::Overridable<void(std::string)>> writeErrorOverride;

        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable spaceAvailable; // wakes writers blocked on a full bounded queue
        std::deque<std::string> queue;
        bool running = false;
        std::thread worker;

        std::mutex stdinClaimMutex;
        std::string stdinClaimedBy; // empty = unclaimed
        bool cliLineReaderInitialized = false;

        // Set (once, before the reader thread starts) when enableStdin()
        // finds a real interactive terminal on stdin -- see
        // isInteractiveTerminal(). Only ever goes false -> true, so
        // write()/writeError() can check it with a relaxed-ish
        // acquire/release pair instead of taking a lock just to decide
        // which output path to use. replxxInstance itself is created
        // before this is set (release) and only ever read by other
        // threads after observing it true (acquire), so by the time any
        // writer dereferences it, construction has already happened-before.
        std::atomic<bool> usingReplxx{ false };
        std::unique_ptr<replxx::Replxx> replxxInstance;
        std::mutex replxxWriteMutex; // serializes concurrent write()/writeError() calls into replxxInstance


#if defined(_WIN32)
        // Console input mode as it was right before replxxInstance took
        // over stdin, captured in enableStdin(). On a normal replxx
        // input() call, replxx itself flips the console into "raw" mode
        // (no echo, no line buffering) for the duration of the call and
        // restores this exact mode before returning -- but if input()
        // throws partway through (see the try/catch in stdinLoop()), its
        // own restore never runs and the console is left stuck in raw
        // mode. restoreConsoleInputModeAfterReplxxFailure() puts this
        // mode back by hand so the std::getline() fallback actually gets
        // echo and line buffering back, instead of silently reading
        // unechoed, byte-at-a-time input that never looks like it's
        // "working".
        bool hasSavedConsoleInputMode = false;
        DWORD savedConsoleInputMode = 0;
#endif

        std::mutex childMutex;
        std::shared_ptr<dsbox_ipc::IpcChannel> childChannel;
        std::atomic<bool> childActive{ false }; // fast-path check for write()
        std::mutex childWriteMutex; // serializes writes to std::cout from the child channel

        mutable std::mutex subprocessesMutex;
        std::unordered_map<std::string, SubprocessEntry> subprocesses;

        void write(std::string text);
        void writeError(std::string text);
        void deliver(std::string text);
        void workerLoop();
        void stdinLoop();
        void childStdinLoop();
        void stopWorker();
        bool claimStdin(std::string_view purpose);
        dsbox_ipc::IpcChannelOptions channelOptions() const;

        // True when stdin is a real terminal a human could be typing
        // into right now, as opposed to a pipe, a redirected file, or
        // (in tests) an in-process std::cin.rdbuf() swap. Checked against
        // the OS-level file descriptor, not std::cin's C++-level buffer,
        // because that's what actually determines whether a line editor
        // can take over the terminal. Only enableStdin() calls this
        // (once, before deciding whether to stand up replxxInstance).
        static bool isInteractiveTerminal();

        // Called from stdinLoop()'s catch blocks, right before falling
        // back to std::getline(). See hasSavedConsoleInputMode's comment
        // for why this is needed on Windows. A no-op (besides clearing
        // std::cin's stream state) on platforms without that failure
        // mode, so stdinLoop() can call it unconditionally.
        void restoreConsoleInputModeAfterReplxxFailure();
    };

    void IOModule::Impl::stopWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            running = false;
        }

        // Wakes the worker if it's currently waiting on an empty queue,
        // and any writer blocked on a full bounded queue, so both can
        // notice `running` is now false.
        cv.notify_all();
        spaceAvailable.notify_all();

        if (worker.joinable())
            worker.join();
    }

    void IOModule::Impl::write(std::string text)
    {
        if (childActive.load())
        {
            // stdout is claimed by the child IPC link -- writing a
            // human-readable log line into the middle of a frame would
            // corrupt the wire, so redirect to stderr instead. See the
            // connectAsChild() docs.
            writeError(std::move(text));
            return;
        }

        std::unique_lock<std::mutex> lock(mutex);

        if (running)
        {
            if (options.maxQueueSize > 0)
            {
                spaceAvailable.wait(lock, [this]
                {
                    return !running || queue.size() < options.maxQueueSize;
                });
            }

            if (!running)
            {
                lock.unlock();
                deliver(std::move(text));
                return;
            }

            queue.push_back(std::move(text));
            lock.unlock();
            cv.notify_one();
            return;
        }

        lock.unlock();

        // No consumer thread running (not started yet, or already shut
        // down) -- deliver synchronously instead of queuing into the
        // void.
        deliver(std::move(text));
    }

    void IOModule::Impl::writeError(std::string text)
    {
        if (writeErrorOverride)
        {
            try
            {
                (*writeErrorOverride)(std::move(text));
            }
            catch (...)
            {
                // A misbehaving override must not be able to crash the
                // caller -- best effort: drop this one message.
            }

            return;
        }

        // Interactive terminal with replxx owning the input line: route
        // through it (same as deliver()'s equivalent branch) instead of
        // std::cerr directly, so an error line erases/redraws the
        // in-progress prompt+buffer exactly like a stdout log line does,
        // rather than being the one stream that still corrupts it.
        if (usingReplxx.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(replxxWriteMutex);
            replxxInstance->print("%s", text.c_str());
            return;
        }

        // onInit() hasn't run yet -- no CommunicationManager reference
        // to check for an override against, so there is nothing to
        // check. Write directly, the same thing the default (no
        // override) path would do anyway.
        std::cerr << text;
    }

    void IOModule::Impl::deliver(std::string text)
    {
        if (writeOverride)
        {
            try
            {
                (*writeOverride)(std::move(text));
            }
            catch (...)
            {
                // A misbehaving override must not be able to take down
                // the worker thread or block subsequent messages -- best
                // effort: drop this one message and keep going.
            }

            return;
        }

        // Interactive terminal with replxx owning the input line
        // (enableStdin() found a real tty): Replxx::print() erases
        // whatever prompt/buffer is currently drawn, writes this text,
        // then redraws the prompt and anything the user had already
        // typed -- instead of a bare std::cout write landing in the
        // middle of it. "%s" + c_str(): text is arbitrary (command
        // output, log lines) and must never be interpreted as a format
        // string. Guarded by the same mutex write()/writeError() share so
        // two concurrent deliveries can't interleave their prompt
        // erase/redraw sequences.
        if (usingReplxx.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(replxxWriteMutex);
            replxxInstance->print("%s", text.c_str());
            return;
        }

        // onInit() hasn't run yet -- see writeError()'s equivalent
        // branch above for why this writes directly instead of routing
        // through the (nonexistent yet) bus.
        std::cout << text;
    }

    void IOModule::Impl::workerLoop()
    {
        while (true)
        {
            std::string message;

            {
                std::unique_lock<std::mutex> lock(mutex);

                cv.wait(lock, [this] { return !running || !queue.empty(); });

                if (queue.empty())
                {
                    // The predicate above only allows waking with an
                    // empty queue when !running -- i.e. fully drained
                    // AND stopping.
                    break;
                }

                message = std::move(queue.front());
                queue.pop_front();
            }

            // A slot just freed up -- wake one writer blocked on a full
            // bounded queue (if any) before delivering, so producers and
            // the worker make progress concurrently instead of in lockstep.
            spaceAvailable.notify_one();

            // Delivered outside the lock: an override is arbitrary user
            // code (possibly slow, possibly re-entering this
            // CommunicationManager) and must never run while holding
            // `mutex`, or a concurrent write() from another thread would
            // block on I/O it has nothing to do with.
            deliver(std::move(message));
        }
    }

    bool IOModule::Impl::claimStdin(std::string_view purpose)
    {
        std::lock_guard<std::mutex> lock(stdinClaimMutex);

        if (stdinClaimedBy.empty())
        {
            stdinClaimedBy = purpose;
            return true;
        }

        if (stdinClaimedBy == purpose)
            return false; // already claimed by this same purpose -- caller treats as "already enabled, no-op"

        throw std::runtime_error(
            "dsbox_io: stdin is already claimed by '" + stdinClaimedBy +
            "', cannot also use it for '" + std::string(purpose) + "'");
    }

    dsbox_ipc::IpcChannelOptions IOModule::Impl::channelOptions() const
    {
        dsbox_ipc::IpcChannelOptions result;

        // Request timeouts of every channel this module creates run on
        // the Context's shared scheduler, if a scheduler module has
        // published one -- one timer thread for all channels instead of
        // one private scheduler per channel that uses a timeout. Looked
        // up per channel, not cached: the scheduler may appear after
        // onInit() (or be withdrawn at shutdown), and a channel that gets
        // null, or a scheduler that has since been stopped, falls back to
        // a private one by itself (see IpcChannelOptions::scheduler).
        if (context)
            result.scheduler = context->get<dsbox_scheduler::Scheduler>();

        return result;
    }

    bool IOModule::Impl::isInteractiveTerminal()
    {
        return DSBOX_IO_ISATTY(DSBOX_IO_FILENO(stdin)) != 0;
    }

    void IOModule::Impl::restoreConsoleInputModeAfterReplxxFailure()
    {
#if defined(_WIN32)
        if (hasSavedConsoleInputMode)
        {
            HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

            if (stdinHandle != INVALID_HANDLE_VALUE && stdinHandle != nullptr)
                SetConsoleMode(stdinHandle, savedConsoleInputMode);

            // One restore attempt is all we get: if this handle is bad
            // or SetConsoleMode fails, retrying on a later failure (e.g.
            // connectAsChild() was never relevant here, but a second
            // enableStdin() can't happen -- claimStdin() is one-shot)
            // won't do any better.
            hasSavedConsoleInputMode = false;
        }
#endif

        // Whatever state std::cin's own fail/eof bits ended up in while
        // replxx was driving the same underlying handle -- clear them so
        // the plain std::getline() loop below starts clean instead of
        // possibly seeing a stream already marked bad.
        std::cin.clear();
    }

    void IOModule::Impl::stdinLoop()
    {
        dsbox::Context* ctx = context;

        if (usingReplxx.load(std::memory_order_relaxed))
        {
            // replxx owns the terminal here: it puts stdin into raw
            // mode, handles its own echo/line-editing, and input()
            // blocks until a full line is ready (or EOF/Ctrl-D), exactly
            // like std::getline() below but with a real line editor
            // behind it. Empty prompt (""): this CLI has never printed
            // one of its own, so nothing changes there.
            //
            // input() is wrapped in try/catch: when the process has no
            // real console attached (e.g. started under a debugger
            // without a console window, or with stdout/stdin handles
            // that look like a tty via _isatty() but aren't backed by a
            // working Win32 console), replxx's own Win32 console calls
            // can throw -- and its internal catch handler's cleanup
            // (finalize_input() -> write8()) can throw a SECOND,
            // unhandled exception while handling the first, which
            // escapes input() entirely and takes the whole process down
            // via std::terminate(). Catching here and falling back to
            // plain std::getline() keeps the process alive instead.
            while (true)
            {
                const char* result = nullptr;

                try
                {
                    result = replxxInstance->input("");
                }
                catch (const std::exception& error)
                {
                    std::fprintf(stderr,
                        "dsbox_io: replxx input() failed (%s) -- falling back to plain stdin\n",
                        error.what());
                    usingReplxx.store(false, std::memory_order_release);
                    restoreConsoleInputModeAfterReplxxFailure();
                    break;
                }
                catch (...)
                {
                    std::fprintf(stderr,
                        "dsbox_io: replxx input() failed with an unrecognized error -- falling back to plain stdin\n");
                    usingReplxx.store(false, std::memory_order_release);
                    restoreConsoleInputModeAfterReplxxFailure();
                    break;
                }

                if (!result)
                {
                    // EOF (Ctrl-D) or a read error -- same end-of-stdin
                    // condition std::getline() hits below, see that
                    // branch's comment.
                    return;
                }

                std::string line(result);

                if (!line.empty())
                    replxxInstance->history_add(line);

                ctx->comm().emit<std::string>("io.stdin.line", line);

            }

            // Fell through from the catch above: usingReplxx is now
            // false, so write()/writeError()/deliver() will route
            // through plain std::cout/std::cerr from here on, matching
            // the std::getline() loop we're about to fall into.
        }

        std::string line;

        while (std::getline(std::cin, line))
        {
            ctx->comm().emit<std::string>("io.stdin.line", line);
        }

        // std::cin hit EOF (or failed) -- nothing left to read, thread
        // exits on its own. This is the only way this thread ever stops
        // short of the whole process exiting; see enableStdin()'s docs.
    }

    void IOModule::Impl::childStdinLoop()
    {
        // Raw bytes, not lines -- the whole point of connectAsChild() is
        // that the payload may be binary/contain embedded '\n', so this
        // must never split on it the way stdinLoop() (line mode) does.
        std::shared_ptr<dsbox_ipc::IpcChannel> channel;
        {
            std::lock_guard<std::mutex> lock(childMutex);
            channel = childChannel;
        }

        if (!channel)
            return;

        std::vector<char> buffer(options.childReadBufferSize > 0 ? options.childReadBufferSize : 1);
        while (std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || std::cin.gcount() > 0)
        {
            const auto count = static_cast<size_t>(std::cin.gcount());
            channel->onBytesReceived(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), count));

            if (!std::cin)
                break;
        }

        channel->close();
        // Same caveat as stdinLoop(): this thread is detached and only
        // stops on stdin EOF or process exit -- see connectAsChild()'s
        // docs and enableStdin()'s more detailed caveat.
    }

    IOModule::IOModule() : IOModule(IOOptions{})
    {
    }

    IOModule::IOModule(IOOptions options)
        : m_impl(std::make_unique<Impl>(std::move(options)))
    {
    }

    IOModule::~IOModule()
    {
        m_impl->stopWorker();

        {
            std::lock_guard<std::mutex> lock(m_impl->childMutex);
            if (m_impl->childChannel)
                m_impl->childChannel->close();
        }

        std::unordered_map<std::string, Impl::SubprocessEntry> subprocesses;
        {
            std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);
            subprocesses.swap(m_impl->subprocesses);
        }

        for (auto& [entryName, entry] : subprocesses)
        {
            (void)entryName;
            if (entry.channel)
                entry.channel->close();
            // entry.process's destructor forcibly terminates it if still
            // running -- see dsbox_process::Subprocess's own docs.
        }
    }

    void IOModule::onInit(dsbox::Context& context)
    {
        m_impl->context = &context;

        // These two lambdas ARE the "no override" path from here on --
        // Overridable always holds a value once onInit() has run, so
        // deliver()/writeError()'s own `if (usingReplxx)` branches below
        // this point never run in a live process; they only matter for
        // write()/writeError() calls made before onInit() (or after
        // onShutdown()). So the replxx redirect has to live here too, not
        // just there.
        Impl* impl = m_impl.get();

        m_impl->writeOverride.emplace(
            context.comm(),
            "io.stdout.write",
            [impl](std::string text)
            {
                if (impl->usingReplxx.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> lock(impl->replxxWriteMutex);
                    impl->replxxInstance->print("%s", text.c_str());
                    return;
                }

                std::cout << text;
            });

        m_impl->writeErrorOverride.emplace(
            context.comm(),
            "io.stderr.write",
            [impl](std::string text)
            {
                if (impl->usingReplxx.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> lock(impl->replxxWriteMutex);
                    impl->replxxInstance->print("%s", text.c_str());
                    return;
                }

                std::cerr << text;
            });

        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->running = true;
        }

        m_impl->worker = std::thread(&IOModule::Impl::workerLoop, m_impl.get());
    }

    void IOModule::onShutdown(dsbox::Context&)
    {
        m_impl->stopWorker();

        {
            std::lock_guard<std::mutex> lock(m_impl->childMutex);
            if (m_impl->childChannel)
                m_impl->childChannel->close();
        }

        std::unordered_map<std::string, Impl::SubprocessEntry> subprocesses;
        {
            std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);
            subprocesses.swap(m_impl->subprocesses);
        }

        for (auto& [entryName, entry] : subprocesses)
        {
            (void)entryName;
            if (entry.channel)
                entry.channel->close();
        }

        // Deliberately NOT touched here: the stdin reader thread (line
        // mode from enableStdin(), or raw-byte mode from
        // connectAsChild()). Both are detached and, per their documented
        // caveats, only safe to use when the Context outlives them.
    }

    void IOModule::write(std::string text)
    {
        m_impl->write(std::move(text));
    }

    void IOModule::writeError(std::string text)
    {
        m_impl->writeError(std::move(text));
    }

    void IOModule::enableStdin()
    {
        if (!m_impl->context)
            return; // onInit() hasn't run -- nothing to emit through yet

        if (!m_impl->claimStdin("line"))
            return; // already enabled


        if (Impl::isInteractiveTerminal())
        {
            // A human is (probably) typing at a real console: hand
            // stdin to replxx so write()/writeError() can erase-and-
            // redraw around an in-progress input line instead of a log
            // line splitting it apart on screen. Piped/redirected stdin
            // (including every unit test, which swaps std::cin's rdbuf
            // rather than its OS-level fd) fails isInteractiveTerminal()
            // and keeps using plain std::getline() below, unchanged.

            bool consoleLooksUsable = true;

#if defined(_WIN32)
            // Captured before replxx ever touches the console, so that
            // if a later input() call throws instead of cleanly
            // restoring this mode itself, stdinLoop()'s catch block can
            // put it back by hand -- see hasSavedConsoleInputMode's
            // comment for why that matters (this is the actual fix for
            // "input just doesn't work" after the fallback kicks in).
            //
            // GetConsoleMode() failing here is itself worth acting on
            // immediately rather than just noted for later: under some
            // ways of launching a console app from the Visual Studio
            // debugger, _isatty() reports true for the stdin handle even
            // though it isn't backed by a real, fully-functional Win32
            // console (e.g. a pseudo-console that doesn't implement the
            // raw-mode calls replxx needs) -- GetConsoleMode() failing is
            // a reliable early tell for that, so when it does, skip
            // replxx entirely and go straight to std::getline() below
            // instead of waiting to hit the same problem inside input().
            {
                HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

                if (stdinHandle != INVALID_HANDLE_VALUE && stdinHandle != nullptr)
                {
                    m_impl->hasSavedConsoleInputMode =
                        GetConsoleMode(stdinHandle, &m_impl->savedConsoleInputMode) != 0;
                }

                consoleLooksUsable = m_impl->hasSavedConsoleInputMode;
            }
#endif

            if (consoleLooksUsable)
            {
                m_impl->replxxInstance = std::make_unique<replxx::Replxx>();

                // Release: by the time any thread observes
                // usingReplxx==true via the acquire loads in
                // write()/writeError()/deliver(), replxxInstance's
                // construction above is guaranteed visible to it too.
                m_impl->usingReplxx.store(true, std::memory_order_release);
            }
        }

        std::thread(&IOModule::Impl::stdinLoop, m_impl.get()).detach();
    }

    std::optional<std::string> IOModule::readStdinLine()
    {
        if (!m_impl->context)
            return std::nullopt;

        {
            std::lock_guard<std::mutex> lock(m_impl->stdinClaimMutex);

            if (m_impl->stdinClaimedBy.empty())
                m_impl->stdinClaimedBy = "CLI line reader";
            else if (m_impl->stdinClaimedBy != "CLI line reader")
                throw std::runtime_error(
                    "dsbox_io: stdin is already claimed by '" + m_impl->stdinClaimedBy +
                    "', cannot also use it for 'CLI line reader'");
        }

        if (!m_impl->cliLineReaderInitialized)
        {
            m_impl->cliLineReaderInitialized = true;

            if (Impl::isInteractiveTerminal())
            {
                bool consoleLooksUsable = true;

#if defined(_WIN32)
                HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
                m_impl->hasSavedConsoleInputMode =
                    stdinHandle != INVALID_HANDLE_VALUE && stdinHandle != nullptr &&
                    GetConsoleMode(stdinHandle, &m_impl->savedConsoleInputMode) != 0;
                consoleLooksUsable = m_impl->hasSavedConsoleInputMode;
#endif

                if (consoleLooksUsable)
                {
                    m_impl->replxxInstance = std::make_unique<replxx::Replxx>();
                    m_impl->usingReplxx.store(true, std::memory_order_release);
                }
            }
        }

        if (m_impl->usingReplxx.load(std::memory_order_acquire))
        {
            try
            {
                const char* result = m_impl->replxxInstance->input("");
                if (!result)
                {
                    // replxx also returns nullptr when the current edit
                    // is aborted (or its terminal read fails). Do not
                    // let that tear down the whole CLI while its console
                    // is still attached; deliver an empty line and let
                    // the next input() call recover. EOF from redirected
                    // stdin is handled by the std::getline() path below.
                    if (Impl::isInteractiveTerminal())
                        return std::string{};

                    return std::nullopt;
                }

                std::string line(result);
                if (!line.empty())
                    m_impl->replxxInstance->history_add(line);
                return line;
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr,
                    "dsbox_io: replxx input() failed (%s) -- falling back to plain stdin\n",
                    error.what());
                m_impl->usingReplxx.store(false, std::memory_order_release);
                m_impl->restoreConsoleInputModeAfterReplxxFailure();
            }
            catch (...)
            {
                std::fprintf(stderr,
                    "dsbox_io: replxx input() failed with an unrecognized error -- falling back to plain stdin\n");
                m_impl->usingReplxx.store(false, std::memory_order_release);
                m_impl->restoreConsoleInputModeAfterReplxxFailure();
            }
        }

        std::string line;
        if (!std::getline(std::cin, line))
            return std::nullopt;
        return line;
    }

    std::shared_ptr<dsbox_ipc::IpcChannel> IOModule::connectAsChild(
        std::unique_ptr<dsbox_ipc::IFrameCodec> codec)
    {
        {
            std::lock_guard<std::mutex> lock(m_impl->childMutex);
            if (m_impl->childChannel)
                throw std::runtime_error("dsbox_io: connectAsChild() has already been called once for this process");
        }

        // Throws if enableStdin() already claimed stdin. Note this is
        // NOT idempotent the way enableStdin() is: a second
        // connectAsChild() call is rejected above regardless, so a
        // repeat call here would only happen as part of that same
        // rejected attempt.
        m_impl->claimStdin("child");

        if (!codec)
            codec = std::make_unique<dsbox_ipc::TextLineFrameCodec>();

        Impl* impl = m_impl.get();

        auto channel = std::make_shared<dsbox_ipc::IpcChannel>(
            std::move(codec),
            [impl](std::span<const std::byte> bytes)
            {
                std::lock_guard<std::mutex> writeLock(impl->childWriteMutex);
                std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                std::cout.flush();
            },
            impl->channelOptions());

        {
            std::lock_guard<std::mutex> lock(m_impl->childMutex);
            m_impl->childChannel = channel;
        }
        m_impl->childActive.store(true);

        std::thread(&IOModule::Impl::childStdinLoop, impl).detach();

        return channel;
    }

    std::shared_ptr<dsbox_ipc::IpcChannel> IOModule::connectSubprocess(
        std::string name,
        dsbox_process::SubprocessOptions options,
        std::unique_ptr<dsbox_ipc::IFrameCodec> codec)
    {
        {
            std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);
            if (m_impl->subprocesses.contains(name))
                throw std::runtime_error("dsbox_io: a subprocess named '" + name + "' is already connected");
        }

        if (!codec)
            codec = std::make_unique<dsbox_ipc::LengthPrefixedFrameCodec>();

        auto process = std::make_unique<dsbox_process::Subprocess>(std::move(options));
        dsbox_process::Subprocess* processPtr = process.get();

        auto channel = std::make_shared<dsbox_ipc::IpcChannel>(
            std::move(codec),
            [processPtr](std::span<const std::byte> bytes)
            {
                processPtr->writeStdin(bytes);
            },
            m_impl->channelOptions());

        process->onStdoutData([channel](std::span<const std::byte> bytes)
        {
            channel->onBytesReceived(bytes);
        });

        // The child's logs flow through this module's own writeError(),
        // exactly like this process's own diagnostics -- no separate log
        // sink needed for a spawned DSBox subprocess.
        Impl* impl = m_impl.get();
        process->onStderrLine([impl, name](std::string line)
        {
            impl->writeError(name + ": " + line + "\n");
        });

        process->onExit([impl, name, channel](int exitCode)
        {
            channel->close();

            if (impl->context)
                impl->context->comm().emit<SubprocessExitEvent>("io.subprocess.exit", SubprocessExitEvent{ name, exitCode });
        });

        {
            std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);
            m_impl->subprocesses.emplace(name, Impl::SubprocessEntry{ std::move(process), channel });
        }

        return channel;
    }

    std::shared_ptr<dsbox_ipc::IpcChannel> IOModule::subprocessChannel(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);

        auto it = m_impl->subprocesses.find(std::string(name));
        if (it == m_impl->subprocesses.end())
            return nullptr;

        return it->second.channel;
    }

    void IOModule::disconnectSubprocess(std::string_view name)
    {
        Impl::SubprocessEntry entry;
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(m_impl->subprocessesMutex);

            auto it = m_impl->subprocesses.find(std::string(name));
            if (it != m_impl->subprocesses.end())
            {
                entry = std::move(it->second);
                m_impl->subprocesses.erase(it);
                found = true;
            }
        }

        if (!found)
            return;

        if (entry.channel)
            entry.channel->close();
        // entry.process's destructor (running here, as `entry` goes out
        // of scope) forcibly terminates it if still running -- see
        // dsbox_process::Subprocess's own docs.
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_io::IOModule)
