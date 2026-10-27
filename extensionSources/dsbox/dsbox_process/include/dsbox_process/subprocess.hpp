#pragma once

#include <dsbox_process/api.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dsbox_process
{
    struct SubprocessOptions
    {
        std::string executable;
        std::vector<std::string> arguments;
        std::optional<std::string> workingDirectory;

        // Extra environment variables for the child process, applied on
        // top of (and overriding, by name) whatever it otherwise
        // inherits -- see inheritParentEnvironment. Variable name
        // matching follows native OS rules: case-insensitive on
        // Windows, case-sensitive on POSIX. An empty vector (the
        // default) adds nothing.
        std::vector<std::pair<std::string, std::string>> environment;

        // If true (the default), the child also inherits every
        // variable from this process's own environment; an entry in
        // `environment` above with the same name overrides the
        // inherited value. If false, the child sees ONLY the entries
        // listed in `environment` -- nothing is inherited, not even
        // things like PATH, so set this to false deliberately (usually
        // together with a non-empty `environment`).
        bool inheritParentEnvironment = true;
    };

    // Owns exactly one spawned child process and its three standard
    // pipes. This class only knows how to move bytes in and out of a
    // process -- it has no idea what those bytes mean (framing,
    // correlation, addressing all live one layer up, in dsbox_ipc).
    //
    // stdout is delivered as raw, unsplit bytes on a dedicated reader
    // thread: it may carry a binary IPC frame whose payload happens to
    // contain '\n', so this class must never impose line-splitting on
    // it. stderr IS split into lines -- the conventional channel for
    // human-readable logs, mirroring how LSP-style servers keep their
    // protocol on stdout and diagnostics on stderr.
    //
    // Lifecycle: the child is spawned synchronously inside the
    // constructor, which throws std::runtime_error if spawning fails.
    // Reader threads (stdout, stderr) and a waiter thread (blocks on
    // process exit) start immediately, before the constructor returns.
    //
    // Caveat -- read before relying on the destructor: if the process
    // is still running when this object is destroyed, the destructor
    // forcibly terminates it (TerminateProcess / SIGKILL) rather than
    // blocking indefinitely on a child that may never exit on its own.
    // Call wait() (after closeStdin(), if the child exits on stdin EOF)
    // if you need a graceful, non-forced shutdown instead.
    //
    // Thread safety: writeStdin()/closeStdin() are safe to call
    // concurrently from any thread. The on*() callback setters are NOT
    // safe to call concurrently with data arriving -- set them right
    // after construction, before doing anything that could make the
    // child produce output (which, since the child is already running
    // by the time the constructor returns, effectively means: set them
    // before returning control to anything that talks to the child).
    class DSBOX_PROCESS_API Subprocess
    {
    public:
        explicit Subprocess(SubprocessOptions options);
        ~Subprocess();

        Subprocess(const Subprocess&) = delete;
        Subprocess& operator=(const Subprocess&) = delete;

        // Called from the stdout reader thread with each chunk as it
        // arrives. Never called concurrently with itself. A misbehaving
        // callback cannot bring down the reader thread: exceptions are
        // caught and logged nowhere (there is no logger here by
        // design) -- the caller should not throw.
        void onStdoutData(std::function<void(std::span<const std::byte>)> callback);

        // Called from the stderr reader thread, once per complete line
        // (the trailing '\n' is stripped). Same exception-safety caveat
        // as onStdoutData.
        void onStderrLine(std::function<void(std::string)> callback);

        // Called exactly once, from the internal waiter thread, the
        // moment the process has exited. Fires even if wait() is never
        // called explicitly.
        void onExit(std::function<void(int exitCode)> callback);

        // Asynchronous: queues the bytes and returns immediately. Safe
        // to call from any thread, including concurrently with itself.
        void writeStdin(std::span<const std::byte> data);

        // Signals EOF on the child's stdin once the write queue has
        // drained. Idempotent.
        void closeStdin();

        bool isRunning() const;

        // Blocks until the process has exited (if it hasn't already)
        // and every reader/writer thread has been joined. Safe to call
        // more than once and from any thread; every caller observes the
        // same exit code.
        int wait();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
