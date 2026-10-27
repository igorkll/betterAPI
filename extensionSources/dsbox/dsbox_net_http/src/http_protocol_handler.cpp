#include <dsbox_net_http/http_protocol_handler.hpp>
#include <dsbox_net_http/http1.hpp>

#include <dsbox_net/body.hpp>
#include <dsbox_net/cancellation.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dsbox_net_http
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        using dsbox_net::BodyPipe;
        using dsbox_net::FetchCallback;
        using dsbox_net::FetchResult;
        using dsbox_net::IByteStream;
        using dsbox_net::NetError;
        using dsbox_net::NetErrorCode;
        using dsbox_net::NetException;
        using dsbox_net::Request;
        using dsbox_net::Response;
        using dsbox_net::StatusClass;
        using dsbox_net::Url;
        using dsbox_net_socket::ConnectOptions;
        using dsbox_net_socket::IConnection;
        using dsbox_net_socket::IConnector;

        constexpr std::size_t kIoBufferSize = 16 * 1024;

        // A redirect response's body is read and thrown away so its
        // connection can be reused -- but only if that is cheap.
        constexpr std::uint64_t kMaxDrainBytes = 64 * 1024;

        NetError makeError(NetErrorCode code, std::string message)
        {
            return NetError{ code, std::move(message), 0 };
        }

        NetException makeException(NetErrorCode code, std::string message)
        {
            return NetException(makeError(code, std::move(message)));
        }

        std::span<const std::byte> asBytes(std::string_view text) noexcept
        {
            return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
        }

        /*
         * Request/response classification helpers
         */

        bool isIdempotentMethod(std::string_view method)
        {
            for (const std::string_view name : { "GET", "HEAD", "OPTIONS", "PUT", "DELETE", "TRACE" })
            {
                if (http1::equalsIgnoreCase(method, name))
                    return true;
            }

            return false;
        }

        // Methods that, when sent without a body, must say so with
        // "Content-Length: 0" (RFC 9110 section 8.6).
        bool methodExpectsBody(std::string_view method)
        {
            for (const std::string_view name : { "POST", "PUT", "PATCH" })
            {
                if (http1::equalsIgnoreCase(method, name))
                    return true;
            }

            return false;
        }

        StatusClass classify(int status)
        {
            if (status >= 200 && status < 300)
                return StatusClass::Success;

            if (status == 404 || status == 410)
                return StatusClass::NotFound;

            if (status == 401 || status == 403 || status == 407)
                return StatusClass::Denied;

            if (status >= 400 && status < 500)
                return StatusClass::ClientError;

            if (status >= 500 && status < 600)
                return StatusClass::ServerError;

            return StatusClass::Other;
        }

        // The value of the Host header: host, plus ":port" only when the
        // port is not the scheme's default.
        std::string buildHostHeader(const Url& url)
        {
            std::string host = url.host;

            if (url.port && *url.port != 0)
            {
                Url defaults;
                defaults.scheme = url.scheme;

                if (*url.port != defaults.effectivePort())
                    host += ":" + std::to_string(*url.port);
            }

            return host;
        }

        std::string buildTarget(const Url& url)
        {
            std::string target = url.path.empty() ? "/" : url.path;

            if (!url.query.empty())
            {
                target += '?';
                target += url.query;
            }

            return target;
        }

        std::string poolKey(const Url& url)
        {
            return url.scheme + "://" + url.host + ":" + std::to_string(url.effectivePort());
        }

        // Turns a 3xx response into a Redirect for NetworkManager to
        // follow, using the browser-compatible method rewriting rules.
        std::optional<dsbox_net::Redirect> redirectFor(const Request& request, const http1::ResponseHead& head)
        {
            const int status = head.status;

            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
                return std::nullopt;

            const auto location = head.headers.get("Location");

            if (!location || location->empty())
                return std::nullopt;

            auto target = request.url.resolve(std::string(*location));

            if (!target)
                return std::nullopt;

            dsbox_net::Redirect redirect;
            redirect.target = std::move(*target);

            if (status == 303)
            {
                redirect.method = http1::equalsIgnoreCase(request.method, "HEAD") ? "HEAD" : "GET";
                redirect.keepBody = false;
            }
            else if ((status == 301 || status == 302) && http1::equalsIgnoreCase(request.method, "POST"))
            {
                redirect.method = "GET";
                redirect.keepBody = false;
            }
            else
            {
                redirect.keepBody = true; // 307/308, or 301/302 of a non-POST: same method, same body
            }

            return redirect;
        }

        /*
         * Transfer: the shared state of one in-flight request
         */

        // Everything that must be reachable from more than one thread
        // for a single request: the cancel callback, the thread running
        // the exchange, the thread streaming the body, and shutdown().
        //
        // Its main job is to make "deliver exactly one result" and
        // "abort" race-free: whichever of complete()/abort() gets there
        // first wins, and the loser finds out and cleans up after
        // itself.
        class Transfer : public std::enable_shared_from_this<Transfer>
        {
        public:
            explicit Transfer(FetchCallback done)
                : m_done(std::move(done))
            {
            }

            // Starts honouring `token`: when it is cancelled the
            // transfer is aborted with the token's reason. If it is
            // already cancelled this aborts before returning.
            void watch(const dsbox_net::CancellationToken& token)
            {
                std::weak_ptr<Transfer> weak = shared_from_this();

                auto subscription = token.onCancel([weak, token]
                {
                    if (auto transfer = weak.lock())
                        transfer->abort(makeError(token.reason(), "request cancelled"));
                });

                std::lock_guard<std::mutex> lock(m_mutex);
                m_subscription = std::move(subscription);
            }

            void unwatch()
            {
                dsbox_net::Subscription subscription;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    subscription = std::move(m_subscription);
                }
                // Destroyed here, outside the lock.
            }

            // Registers the connection used for the exchange so abort()
            // can close it. If the transfer has already been aborted the
            // connection is closed at once.
            void setConnection(std::shared_ptr<IConnection> connection)
            {
                bool aborted = false;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_connection = connection;
                    aborted = m_aborted;
                }

                if (aborted && connection)
                    connection->close();
            }

            // Forgets the connection WITHOUT closing it -- used when
            // handing it back to the pool, after which abort() must no
            // longer touch it.
            void detachConnection()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_connection.reset();
            }

            void closeConnection()
            {
                std::shared_ptr<IConnection> connection;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    connection = std::move(m_connection);
                }

                if (connection)
                    connection->close();
            }

            bool isAborted() const
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_aborted;
            }

            NetError abortError() const
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                return m_abortError;
            }

            // Delivers `result` to the caller, once. `writer`, if given,
            // is the producer side of result.response.body so that a
            // later abort() can fail it. Returns false (delivering
            // nothing) if the transfer was already completed or aborted.
            bool complete(FetchResult result, std::optional<BodyPipe::Writer> writer)
            {
                FetchCallback done;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    if (m_completed)
                        return false;

                    m_completed = true;
                    m_writer = std::move(writer);
                    done = std::move(m_done);
                }

                if (done)
                    done(std::move(result));

                return true;
            }

            bool fail(NetError error)
            {
                FetchResult result;
                result.error = std::move(error);
                return complete(std::move(result), std::nullopt);
            }

            // Cancels the transfer: if no result has been delivered yet,
            // delivers `error`; if a body is being streamed, fails it
            // with `error`. In both cases the connection is closed so
            // any blocked I/O returns. Idempotent.
            void abort(NetError error)
            {
                std::shared_ptr<IConnection> connection;
                std::optional<BodyPipe::Writer> writer;
                FetchCallback done;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    if (m_aborted)
                        return;

                    m_aborted = true;
                    m_abortError = error;
                    connection = m_connection;

                    if (!m_completed)
                    {
                        m_completed = true;
                        done = std::move(m_done);
                    }
                    else
                    {
                        writer = m_writer;
                    }
                }

                // Fail the body BEFORE closing the connection: closing
                // wakes the streaming thread, and it must find the body
                // already failed with the real reason rather than
                // reporting a generic "connection closed".
                if (writer)
                    writer->fail(error);

                if (connection)
                    connection->close();

                if (done)
                {
                    FetchResult result;
                    result.error = std::move(error);
                    done(std::move(result));
                }
            }

        private:
            mutable std::mutex m_mutex;
            FetchCallback m_done;
            bool m_completed = false;
            bool m_aborted = false;
            NetError m_abortError;
            std::shared_ptr<IConnection> m_connection;
            std::optional<BodyPipe::Writer> m_writer;
            dsbox_net::Subscription m_subscription;
        };

        /*
         * ThreadSet: the threads a handler owns
         */

        class ThreadSet
        {
        public:
            ~ThreadSet()
            {
                beginStop();
                joinAll();
            }

            // Runs `work` on a new thread. Returns false (running
            // nothing) once beginStop() has been called.
            bool spawn(std::function<void()> work)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_stopping)
                    return false;

                reapFinished();

                auto finished = std::make_shared<std::atomic<bool>>(false);

                try
                {
                    m_entries.emplace_back();
                    Entry& entry = m_entries.back();
                    entry.finished = finished;
                    entry.thread = std::thread([work = std::move(work), finished]() mutable
                    {
                        work();
                        work = nullptr; // release captured state before announcing we are done
                        finished->store(true);
                    });
                }
                catch (...)
                {
                    if (!m_entries.empty() && !m_entries.back().thread.joinable())
                        m_entries.pop_back();

                    return false;
                }

                return true;
            }

            void beginStop()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stopping = true;
            }

            // Waits for every thread, including ones started while we
            // wait. Call beginStop() first or it may never converge.
            void joinAll()
            {
                for (;;)
                {
                    std::list<Entry> batch;

                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        batch.swap(m_entries);
                    }

                    if (batch.empty())
                        return;

                    for (Entry& entry : batch)
                    {
                        if (!entry.thread.joinable())
                            continue;

                        if (entry.thread.get_id() == std::this_thread::get_id())
                            entry.thread.detach(); // joining ourselves would deadlock
                        else
                            entry.thread.join();
                    }
                }
            }

        private:
            struct Entry
            {
                std::thread thread;
                std::shared_ptr<std::atomic<bool>> finished;
            };

            // Joins and drops threads that have already finished, so a
            // long-lived handler does not accumulate dead entries.
            void reapFinished()
            {
                for (auto it = m_entries.begin(); it != m_entries.end();)
                {
                    if (it->finished && it->finished->load() && it->thread.joinable())
                    {
                        it->thread.join();
                        it = m_entries.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            std::mutex m_mutex;
            std::list<Entry> m_entries;
            bool m_stopping = false;
        };

        /*
         * ConnectionPool: idle keep-alive connections
         */

        class ConnectionPool
        {
        public:
            ConnectionPool(std::size_t maxPerHost, std::size_t maxTotal, std::chrono::milliseconds idleTimeout)
                : m_maxPerHost(maxPerHost)
                , m_maxTotal(maxTotal)
                , m_idleTimeout(idleTimeout)
            {
            }

            // The most recently used live idle connection for `key`.
            std::shared_ptr<IConnection> take(const std::string& key)
            {
                std::vector<std::shared_ptr<IConnection>> discarded;
                std::shared_ptr<IConnection> result;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto now = Clock::now();

                    auto it = m_idle.find(key);

                    if (it != m_idle.end())
                    {
                        auto& list = it->second;

                        while (!list.empty())
                        {
                            Idle idle = std::move(list.back());
                            list.pop_back();
                            --m_total;

                            if (isExpired(idle, now))
                            {
                                discarded.push_back(std::move(idle.connection));
                                continue;
                            }

                            result = std::move(idle.connection);
                            break;
                        }

                        if (list.empty())
                            m_idle.erase(it);
                    }
                }

                closeAll(discarded);
                return result;
            }

            // Takes ownership of a connection whose last response was
            // read completely and which the server left open.
            void put(const std::string& key, std::shared_ptr<IConnection> connection)
            {
                if (!connection)
                    return;

                std::vector<std::shared_ptr<IConnection>> discarded;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto now = Clock::now();

                    pruneExpired(now, discarded);

                    auto& list = m_idle[key];

                    while (list.size() >= m_maxPerHost && !list.empty())
                    {
                        discarded.push_back(std::move(list.front().connection));
                        list.pop_front();
                        --m_total;
                    }

                    if (m_maxPerHost == 0 || m_total >= m_maxTotal)
                    {
                        discarded.push_back(std::move(connection));

                        if (list.empty())
                            m_idle.erase(key);
                    }
                    else
                    {
                        list.push_back(Idle{ std::move(connection), now });
                        ++m_total;
                    }
                }

                closeAll(discarded);
            }

            void clear()
            {
                std::vector<std::shared_ptr<IConnection>> discarded;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);

                    for (auto& [key, list] : m_idle)
                    {
                        for (Idle& idle : list)
                            discarded.push_back(std::move(idle.connection));
                    }

                    m_idle.clear();
                    m_total = 0;
                }

                closeAll(discarded);
            }

        private:
            struct Idle
            {
                std::shared_ptr<IConnection> connection;
                Clock::time_point since;
            };

            bool isExpired(const Idle& idle, Clock::time_point now) const
            {
                return m_idleTimeout.count() > 0 && now - idle.since >= m_idleTimeout;
            }

            void pruneExpired(Clock::time_point now, std::vector<std::shared_ptr<IConnection>>& discarded)
            {
                for (auto it = m_idle.begin(); it != m_idle.end();)
                {
                    auto& list = it->second;

                    while (!list.empty() && isExpired(list.front(), now))
                    {
                        discarded.push_back(std::move(list.front().connection));
                        list.pop_front();
                        --m_total;
                    }

                    if (list.empty())
                        it = m_idle.erase(it);
                    else
                        ++it;
                }
            }

            static void closeAll(std::vector<std::shared_ptr<IConnection>>& connections)
            {
                for (auto& connection : connections)
                {
                    if (connection)
                        connection->close();
                }
            }

            std::size_t m_maxPerHost;
            std::size_t m_maxTotal;
            std::chrono::milliseconds m_idleTimeout;

            std::mutex m_mutex;
            std::map<std::string, std::deque<Idle>> m_idle;
            std::size_t m_total = 0;
        };

        /*
         * Wire exchange helpers
         */

        // Everything about the request that is fixed before the first
        // connection attempt, so a retry does not redo it.
        struct PreparedRequest
        {
            std::string head;                 // request line + headers + blank line
            std::shared_ptr<dsbox_net::IBodySource> body;
            std::optional<std::uint64_t> bodySize;
        };

        void writeAll(IConnection& connection, std::span<const std::byte> data)
        {
            connection.write(data);
        }

        void sendRequest(IConnection& connection, const PreparedRequest& prepared)
        {
            writeAll(connection, asBytes(prepared.head));

            if (!prepared.body)
                return;

            const std::unique_ptr<IByteStream> stream = prepared.body->open();
            std::vector<std::byte> buffer(kIoBufferSize);
            std::uint64_t sent = 0;

            struct StreamCloser
            {
                IByteStream* stream;
                ~StreamCloser() { stream->close(); }
            } closer{ stream.get() };

            for (;;)
            {
                const std::size_t n = stream->read(buffer);

                if (n == 0)
                    break;

                const std::span<const std::byte> chunk(buffer.data(), n);

                if (prepared.bodySize)
                {
                    sent += n;

                    if (sent > *prepared.bodySize)
                        throw makeException(NetErrorCode::Internal, "request body is longer than its declared size");

                    writeAll(connection, chunk);
                }
                else
                {
                    // Unknown size: chunked transfer coding.
                    std::string header = http1::chunkHeader(n);
                    std::vector<std::byte> frame;
                    frame.reserve(header.size() + n + 2);

                    const auto headerBytes = asBytes(header);
                    frame.insert(frame.end(), headerBytes.begin(), headerBytes.end());
                    frame.insert(frame.end(), chunk.begin(), chunk.end());
                    frame.push_back(static_cast<std::byte>('\r'));
                    frame.push_back(static_cast<std::byte>('\n'));

                    writeAll(connection, frame);
                }
            }

            if (prepared.bodySize)
            {
                if (sent != *prepared.bodySize)
                    throw makeException(NetErrorCode::Internal, "request body is shorter than its declared size");
            }
            else
            {
                writeAll(connection, asBytes(http1::kFinalChunk));
            }
        }

        // Reads until a complete (non-informational) response head has
        // arrived. Bytes that followed the head are returned in
        // `leftover`; `sawByte` reports whether the peer sent anything
        // at all, which decides whether a dead reused connection may be
        // retried.
        http1::ResponseHead readResponseHead(
            IConnection& connection,
            std::size_t maxHeadBytes,
            std::vector<std::byte>& leftover,
            bool& sawByte)
        {
            http1::HeadLimits limits;
            limits.maxHeadBytes = maxHeadBytes;

            http1::ResponseHeadParser parser(limits);
            std::vector<std::byte> buffer(kIoBufferSize);
            std::vector<std::byte> carry;

            for (;;)
            {
                while (!carry.empty())
                {
                    std::size_t consumed = 0;
                    const auto status = parser.feed(carry, consumed);
                    carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(consumed));

                    if (status == http1::ResponseHeadParser::Status::Error)
                        throw makeException(NetErrorCode::Protocol, "malformed HTTP response: " + parser.error());

                    if (status == http1::ResponseHeadParser::Status::NeedMore)
                        continue; // carry is empty now; the loop ends and we read more

                    // Complete.
                    const int code = parser.head().status;

                    if (code == 101)
                        throw makeException(NetErrorCode::Protocol, "unexpected protocol upgrade (101)");

                    if (code >= 100 && code < 200)
                    {
                        // Interim response (100 Continue, 103 Early
                        // Hints, ...): not the answer, keep reading.
                        parser.reset();
                        continue;
                    }

                    leftover = std::move(carry);
                    return parser.head();
                }

                const std::size_t n = connection.read(buffer);

                if (n == 0)
                {
                    throw makeException(
                        NetErrorCode::ConnectionReset,
                        sawByte ? "connection closed in the middle of the response head"
                                : "connection closed before any response was received");
                }

                sawByte = true;
                carry.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }

        // Reads and discards a small response body so the connection
        // can be reused. Returns false if that is not possible or not
        // worth it.
        bool drainBody(IConnection& connection, const std::vector<std::byte>& leftover, const http1::BodyFraming& framing)
        {
            try
            {
                switch (framing.kind)
                {
                case http1::FramingKind::None:
                    return leftover.empty();

                case http1::FramingKind::ContentLength:
                {
                    if (framing.length > kMaxDrainBytes || leftover.size() > framing.length)
                        return false;

                    std::uint64_t remaining = framing.length - leftover.size();
                    std::vector<std::byte> buffer(kIoBufferSize);

                    while (remaining > 0)
                    {
                        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
                        const std::size_t n = connection.read(std::span<std::byte>(buffer.data(), want));

                        if (n == 0)
                            return false;

                        remaining -= n;
                    }

                    return true;
                }

                default:
                    return false;
                }
            }
            catch (const NetException&)
            {
                return false;
            }
        }

        // What the body-streaming thread needs. (An aggregate: the
        // writer has no default constructor, so a PumpJob is always
        // built with all of its members at once.)
        struct PumpJob
        {
            std::shared_ptr<Transfer> transfer;
            std::shared_ptr<IConnection> connection;
            std::string poolKey;
            std::vector<std::byte> leftover;
            http1::BodyFraming framing;
            bool reusable;
            BodyPipe::Writer writer;
        };

        enum class PumpOutcome
        {
            Complete,   // body fully delivered, connection left clean
            ReaderGone, // the consumer stopped reading; connection is mid-body
        };
    }

    /*
     * HttpProtocolHandler::Impl
     */

    struct HttpProtocolHandler::Impl
    {
        Impl(ConnectorResolver resolverIn, HttpOptions optionsIn)
            : resolver(std::move(resolverIn))
            , options(std::move(optionsIn))
            , pool(options.maxIdleConnectionsPerHost, options.maxIdleConnections, options.idleConnectionTimeout)
        {
        }

        ConnectorResolver resolver;
        HttpOptions options;
        ConnectionPool pool;
        ThreadSet threads;

        std::mutex activeMutex;
        std::vector<std::weak_ptr<Transfer>> active;

        void registerActive(const std::shared_ptr<Transfer>& transfer)
        {
            std::lock_guard<std::mutex> lock(activeMutex);

            active.erase(
                std::remove_if(active.begin(), active.end(), [](const std::weak_ptr<Transfer>& weak)
                {
                    return weak.expired();
                }),
                active.end());

            active.push_back(transfer);
        }

        void startTransfer(const std::shared_ptr<Transfer>& transfer, Request request)
        {
            registerActive(transfer);

            // May abort (and call `done`) right here if the request was
            // cancelled before it even started.
            transfer->watch(request.cancel);

            if (transfer->isAborted())
            {
                transfer->unwatch();
                return;
            }

            const bool started = threads.spawn([this, transfer, request = std::move(request)]() mutable
            {
                run(transfer, std::move(request));
            });

            if (!started)
            {
                transfer->fail(makeError(NetErrorCode::Cancelled, "HTTP handler is shutting down"));
                transfer->unwatch();
            }
        }

        void shutdown()
        {
            threads.beginStop();

            std::vector<std::shared_ptr<Transfer>> live;

            {
                std::lock_guard<std::mutex> lock(activeMutex);

                for (const auto& weak : active)
                {
                    if (auto transfer = weak.lock())
                        live.push_back(std::move(transfer));
                }

                active.clear();
            }

            for (const auto& transfer : live)
                transfer->abort(makeError(NetErrorCode::Cancelled, "HTTP handler shut down"));

            live.clear();
            threads.joinAll();
            pool.clear();
        }

        // Thread entry point for one request.
        void run(const std::shared_ptr<Transfer>& transfer, Request request)
        {
            bool pumpStarted = false;

            try
            {
                execute(transfer, request, pumpStarted);
            }
            catch (const NetException& exception)
            {
                transfer->fail(exception.error());
            }
            catch (const std::exception& exception)
            {
                transfer->fail(makeError(NetErrorCode::Internal, exception.what()));
            }
            catch (...)
            {
                transfer->fail(makeError(NetErrorCode::Internal, "unknown error while performing HTTP request"));
            }

            // If the body thread took over, it owns the connection and
            // the cancel subscription from here on.
            if (!pumpStarted)
            {
                transfer->closeConnection();
                transfer->unwatch();
            }
        }

        // Runs the exchange up to delivering the response head. Throws
        // NetException on failure; `pumpStarted` is set once the body
        // thread has been launched.
        void execute(const std::shared_ptr<Transfer>& transfer, const Request& request, bool& pumpStarted)
        {
            const Url& url = request.url;

            if (url.scheme != "http" && url.scheme != "https")
            {
                throw makeException(
                    NetErrorCode::UnsupportedScheme,
                    "HttpProtocolHandler cannot handle the scheme '" + url.scheme + "'");
            }

            if (url.host.empty())
                throw makeException(NetErrorCode::InvalidUrl, "URL has no host");

            const std::shared_ptr<IConnector> connector = resolver ? resolver(url.scheme) : nullptr;

            if (!connector)
            {
                throw makeException(
                    NetErrorCode::UnsupportedOperation,
                    "no connector is available for '" + url.scheme + "' (is the transport module loaded?)");
            }

            const PreparedRequest prepared = prepare(request);
            const std::string key = poolKey(url);
            const bool replayable = isIdempotentMethod(request.method);

            for (int attempt = 0;; ++attempt)
            {
                if (transfer->isAborted())
                    throw NetException(transfer->abortError());

                std::shared_ptr<IConnection> connection;
                bool reused = false;

                if (options.keepAlive)
                {
                    connection = pool.take(key);
                    reused = connection != nullptr;
                }

                if (!connection)
                {
                    ConnectOptions connectOptions;
                    connectOptions.timeout = options.connectTimeout;
                    connectOptions.cancel = request.cancel;

                    connection = connector->connect(url.host, url.effectivePort(), connectOptions);
                }

                transfer->setConnection(connection);

                http1::ResponseHead head;
                std::vector<std::byte> leftover;
                bool sawResponseByte = false;

                try
                {
                    sendRequest(*connection, prepared);
                    head = readResponseHead(*connection, options.maxHeadBytes, leftover, sawResponseByte);
                }
                catch (const NetException&)
                {
                    // A pooled connection the server quietly closed
                    // while it sat idle looks exactly like this. Try
                    // once more on a fresh connection -- but only when
                    // repeating the request is safe.
                    if (reused && attempt == 0 && !sawResponseByte && replayable && !transfer->isAborted())
                    {
                        transfer->closeConnection();
                        continue;
                    }

                    throw;
                }

                handleResponse(transfer, request, connection, key, std::move(head), std::move(leftover), pumpStarted);
                return;
            }
        }

        PreparedRequest prepare(const Request& request) const
        {
            PreparedRequest prepared;
            prepared.body = request.body;

            if (prepared.body)
                prepared.bodySize = prepared.body->size();

            const std::string target = http1::encodeRequestTarget(buildTarget(request.url));
            const std::string hostHeader = buildHostHeader(request.url);

            http1::RequestHead head;
            head.method = request.method;
            head.target = target;
            head.host = hostHeader;
            head.headers = &request.metadata;
            head.keepAlive = options.keepAlive;
            head.userAgent = options.userAgent;

            if (prepared.body)
            {
                if (prepared.bodySize)
                    head.contentLength = prepared.bodySize;
                else
                    head.chunkedBody = true;
            }
            else if (methodExpectsBody(request.method))
            {
                head.contentLength = 0;
            }

            prepared.head = http1::serializeRequestHead(head);
            return prepared;
        }

        // Hands a connection that is in a clean state back to the pool.
        void recycle(const std::shared_ptr<Transfer>& transfer, const std::string& key, const std::shared_ptr<IConnection>& connection)
        {
            transfer->detachConnection();
            pool.put(key, connection);
        }

        void handleResponse(
            const std::shared_ptr<Transfer>& transfer,
            const Request& request,
            const std::shared_ptr<IConnection>& connection,
            const std::string& key,
            http1::ResponseHead head,
            std::vector<std::byte> leftover,
            bool& pumpStarted)
        {
            std::string framingError;
            const auto framing = http1::determineFraming(request.method, head, framingError);

            if (!framing)
                throw makeException(NetErrorCode::Protocol, "malformed HTTP response: " + framingError);

            bool reusable = options.keepAlive && http1::connectionStaysOpen(head);

            Response response;
            response.url = request.url;
            response.status = head.status;
            response.statusClass = classify(head.status);
            response.statusText = head.reason;
            response.metadata = head.headers;

            // Chunked coding is undone here, so the body the caller
            // reads is no longer chunked.
            if (framing->kind == http1::FramingKind::Chunked)
                response.metadata.remove("Transfer-Encoding");

            // Redirect: NetworkManager follows it and never reads this
            // response's body, so settle the connection now.
            if (auto redirect = redirectFor(request, head))
            {
                response.redirect = std::move(redirect);

                if (reusable && drainBody(*connection, leftover, *framing))
                    recycle(transfer, key, connection);
                else
                    transfer->closeConnection();

                FetchResult result;
                result.response = std::move(response);
                transfer->complete(std::move(result), std::nullopt);
                return;
            }

            const bool isHead = http1::equalsIgnoreCase(request.method, "HEAD");
            const bool bodyless = framing->kind == http1::FramingKind::None
                || (framing->kind == http1::FramingKind::ContentLength && framing->length == 0);

            if (bodyless)
            {
                if (reusable && leftover.empty())
                    recycle(transfer, key, connection);
                else
                    transfer->closeConnection();

                // Every response except to HEAD gets a body stream --
                // an empty one here -- so callers need not special-case
                // "200 with Content-Length: 0".
                if (!isHead)
                {
                    BodyPipe pipe = BodyPipe::create();
                    pipe.writer().finish();
                    response.body = pipe.reader();
                }

                FetchResult result;
                result.response = std::move(response);
                transfer->complete(std::move(result), std::nullopt);
                return;
            }

            BodyPipe pipe = BodyPipe::create(options.bodyPipeCapacity);

            PumpJob job{
                transfer,
                connection,
                key,
                std::move(leftover),
                *framing,
                reusable && framing->kind != http1::FramingKind::UntilClose,
                pipe.writer()
            };

            // Start streaming BEFORE handing the response over, on its
            // own thread: `done` may read the body right on the thread
            // that calls it.
            const bool started = threads.spawn([this, job = std::move(job)]() mutable
            {
                pump(std::move(job));
            });

            if (!started)
                throw makeException(NetErrorCode::Cancelled, "HTTP handler is shutting down");

            pumpStarted = true;

            response.body = pipe.reader();

            FetchResult result;
            result.response = std::move(response);
            transfer->complete(std::move(result), pipe.writer());
        }

        // Body-streaming thread entry point.
        void pump(PumpJob job)
        {
            bool clean = false;

            try
            {
                clean = copyBody(job) == PumpOutcome::Complete;
            }
            catch (const NetException& exception)
            {
                failBody(job, exception.error());
            }
            catch (const std::exception& exception)
            {
                failBody(job, makeError(NetErrorCode::Internal, exception.what()));
            }
            catch (...)
            {
                failBody(job, makeError(NetErrorCode::Internal, "unknown error while reading HTTP body"));
            }

            if (clean && job.reusable && !job.transfer->isAborted())
                recycle(job.transfer, job.poolKey, job.connection);
            else
                job.transfer->closeConnection();

            job.transfer->unwatch();
        }

        static void failBody(PumpJob& job, NetError error)
        {
            // If the transfer was aborted, the abort's reason (Timeout,
            // Cancelled, ...) is the real cause, not whatever I/O error
            // closing the connection provoked.
            if (job.transfer->isAborted())
                error = job.transfer->abortError();

            job.writer.fail(std::move(error));
        }

        PumpOutcome copyBody(PumpJob& job)
        {
            IConnection& connection = *job.connection;
            BodyPipe::Writer& writer = job.writer;
            std::vector<std::byte> buffer(kIoBufferSize);

            switch (job.framing.kind)
            {
            case http1::FramingKind::None:
                writer.finish();
                return PumpOutcome::Complete;

            case http1::FramingKind::ContentLength:
            {
                std::uint64_t remaining = job.framing.length;

                if (!job.leftover.empty())
                {
                    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, job.leftover.size()));

                    if (take < job.leftover.size())
                        job.reusable = false; // the server sent more than it promised

                    if (take > 0 && !writer.write(std::span<const std::byte>(job.leftover.data(), take)))
                        return PumpOutcome::ReaderGone;

                    remaining -= take;
                }

                while (remaining > 0)
                {
                    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
                    const std::size_t n = connection.read(std::span<std::byte>(buffer.data(), want));

                    if (n == 0)
                    {
                        throw makeException(
                            NetErrorCode::ConnectionReset,
                            "connection closed before the whole response body arrived");
                    }

                    if (!writer.write(std::span<const std::byte>(buffer.data(), n)))
                        return PumpOutcome::ReaderGone;

                    remaining -= n;
                }

                writer.finish();
                return PumpOutcome::Complete;
            }

            case http1::FramingKind::Chunked:
            {
                http1::ChunkedDecoder decoder;
                std::vector<std::byte> decoded;
                bool done = false;

                // Returns false if the consumer has stopped reading.
                const auto consume = [&](std::span<const std::byte> input) -> bool
                {
                    std::size_t consumed = 0;
                    decoded.clear();

                    const auto status = decoder.feed(input, decoded, consumed);

                    if (!decoded.empty() && !writer.write(decoded))
                        return false;

                    if (status == http1::ChunkedDecoder::Status::Error)
                        throw makeException(NetErrorCode::Protocol, "malformed chunked response body: " + decoder.error());

                    if (status == http1::ChunkedDecoder::Status::Done)
                    {
                        done = true;

                        if (consumed < input.size())
                            job.reusable = false; // bytes after the end of the message
                    }

                    return true;
                };

                if (!job.leftover.empty() && !consume(job.leftover))
                    return PumpOutcome::ReaderGone;

                while (!done)
                {
                    const std::size_t n = connection.read(buffer);

                    if (n == 0)
                    {
                        throw makeException(
                            NetErrorCode::ConnectionReset,
                            "connection closed before the chunked response body ended");
                    }

                    if (!consume(std::span<const std::byte>(buffer.data(), n)))
                        return PumpOutcome::ReaderGone;
                }

                writer.finish();
                return PumpOutcome::Complete;
            }

            case http1::FramingKind::UntilClose:
            {
                if (!job.leftover.empty() && !writer.write(job.leftover))
                    return PumpOutcome::ReaderGone;

                for (;;)
                {
                    const std::size_t n = connection.read(buffer);

                    if (n == 0)
                        break;

                    if (!writer.write(std::span<const std::byte>(buffer.data(), n)))
                        return PumpOutcome::ReaderGone;
                }

                // Closed by the peer or by an abort; abort() has already
                // failed the writer in the latter case, which makes
                // finish() a no-op.
                writer.finish();
                job.reusable = false;
                return PumpOutcome::Complete;
            }
            }

            return PumpOutcome::ReaderGone;
        }
    };

    HttpProtocolHandler::HttpProtocolHandler(ConnectorResolver resolver, HttpOptions options)
        : m_impl(std::make_unique<Impl>(std::move(resolver), std::move(options)))
    {
    }

    HttpProtocolHandler::~HttpProtocolHandler()
    {
        shutdown();
    }

    void HttpProtocolHandler::start(dsbox_net::Request request, dsbox_net::FetchCallback done)
    {
        auto transfer = std::make_shared<Transfer>(std::move(done));
        m_impl->startTransfer(transfer, std::move(request));
    }

    void HttpProtocolHandler::shutdown()
    {
        m_impl->shutdown();
    }
}
