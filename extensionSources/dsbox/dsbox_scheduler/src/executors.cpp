#include <dsbox_scheduler/executor.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dsbox_scheduler
{
    // ---------------------------------------------------------------
    // ThreadPoolExecutor
    // ---------------------------------------------------------------

    namespace detail
    {
        struct PoolState
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::condition_variable idleCv;
            std::deque<Callback> queue;
            std::vector<std::thread> workers;
            std::size_t threads = 1;
            std::size_t active = 0;
            bool started = false;
            bool stopping = false;
            ErrorHandler onError;
        };

        // Workers own a reference to the state, so it stays alive even
        // if the executor object is destroyed from one of its own
        // callbacks (that worker gets detached, see shutdown()).
        void poolWorker(std::shared_ptr<PoolState> state)
        {
            PoolState& s = *state;

            for (;;)
            {
                Callback callback;

                {
                    std::unique_lock lock(s.mutex);
                    s.cv.wait(lock, [&] { return s.stopping || !s.queue.empty(); });

                    if (s.queue.empty())
                        return; // stopping, and nothing left to drain

                    callback = std::move(s.queue.front());
                    s.queue.pop_front();
                    ++s.active;
                }

                invokeGuarded(callback, s.onError);
                callback = nullptr; // destroy captures before reporting idle

                {
                    std::lock_guard lock(s.mutex);
                    --s.active;

                    if (s.queue.empty() && s.active == 0)
                        s.idleCv.notify_all();
                }
            }
        }
    }

    ThreadPoolExecutor::ThreadPoolExecutor(std::size_t threads, ErrorHandler onError)
        : m_state(std::make_shared<detail::PoolState>())
    {
        if (threads == 0)
            threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());

        m_state->threads = threads;
        m_state->onError = std::move(onError);
    }

    ThreadPoolExecutor::~ThreadPoolExecutor()
    {
        shutdown();
    }

    bool ThreadPoolExecutor::post(Callback callback)
    {
        if (!callback)
            return true;

        detail::PoolState& s = *m_state;

        {
            std::lock_guard lock(s.mutex);

            if (s.stopping)
                return false;

            if (!s.started)
            {
                s.started = true;
                s.workers.reserve(s.threads);

                for (std::size_t i = 0; i < s.threads; ++i)
                    s.workers.emplace_back(detail::poolWorker, m_state);
            }

            s.queue.push_back(std::move(callback));
        }

        s.cv.notify_one();
        return true;
    }

    void ThreadPoolExecutor::shutdown()
    {
        detail::PoolState& s = *m_state;
        std::vector<std::thread> workers;

        {
            std::lock_guard lock(s.mutex);
            s.stopping = true;
            workers.swap(s.workers);
        }

        s.cv.notify_all();

        const auto self = std::this_thread::get_id();

        for (auto& worker : workers)
        {
            if (worker.get_id() == self)
                worker.detach();
            else
                worker.join();
        }
    }

    void ThreadPoolExecutor::waitIdle()
    {
        detail::PoolState& s = *m_state;

        std::unique_lock lock(s.mutex);
        s.idleCv.wait(lock, [&] { return s.queue.empty() && s.active == 0; });
    }

    std::size_t ThreadPoolExecutor::threadCount() const noexcept
    {
        return m_state->threads;
    }

    // ---------------------------------------------------------------
    // LoopExecutor
    // ---------------------------------------------------------------

    namespace detail
    {
        struct LoopState
        {
            mutable std::mutex mutex;
            std::condition_variable cv;
            std::deque<Callback> queue;
            bool stopRequested = false;
            bool closed = false;
            ErrorHandler onError;
        };
    }

    LoopExecutor::LoopExecutor(ErrorHandler onError)
        : m_state(std::make_shared<detail::LoopState>())
    {
        m_state->onError = std::move(onError);
    }

    LoopExecutor::~LoopExecutor() = default;

    bool LoopExecutor::post(Callback callback)
    {
        if (!callback)
            return true;

        {
            std::lock_guard lock(m_state->mutex);

            if (m_state->closed)
                return false;

            m_state->queue.push_back(std::move(callback));
        }

        m_state->cv.notify_one();
        return true;
    }

    bool LoopExecutor::runOne()
    {
        Callback callback;

        {
            std::lock_guard lock(m_state->mutex);

            if (m_state->queue.empty())
                return false;

            callback = std::move(m_state->queue.front());
            m_state->queue.pop_front();
        }

        detail::invokeGuarded(callback, m_state->onError);
        return true;
    }

    std::size_t LoopExecutor::poll()
    {
        std::size_t count = 0;

        while (runOne())
            ++count;

        return count;
    }

    std::size_t LoopExecutor::runFor(Duration timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::size_t count = 0;

        for (;;)
        {
            Callback callback;

            {
                std::unique_lock lock(m_state->mutex);

                if (std::chrono::steady_clock::now() >= deadline)
                    break;

                m_state->cv.wait_until(
                    lock,
                    deadline,
                    [&] { return m_state->stopRequested || !m_state->queue.empty(); });

                if (m_state->stopRequested || m_state->queue.empty())
                    break;

                callback = std::move(m_state->queue.front());
                m_state->queue.pop_front();
            }

            detail::invokeGuarded(callback, m_state->onError);
            ++count;
        }

        return count;
    }

    std::size_t LoopExecutor::run()
    {
        std::size_t count = 0;

        for (;;)
        {
            Callback callback;

            {
                std::unique_lock lock(m_state->mutex);
                m_state->cv.wait(
                    lock,
                    [&] { return m_state->stopRequested || !m_state->queue.empty(); });

                if (m_state->stopRequested)
                    break;

                callback = std::move(m_state->queue.front());
                m_state->queue.pop_front();
            }

            detail::invokeGuarded(callback, m_state->onError);
            ++count;
        }

        return count;
    }

    void LoopExecutor::stop()
    {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stopRequested = true;
        }

        m_state->cv.notify_all();
    }

    void LoopExecutor::restart()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stopRequested = false;
    }

    void LoopExecutor::close()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->closed = true;
    }

    std::size_t LoopExecutor::pending() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->queue.size();
    }

    // ---------------------------------------------------------------
    // SerialExecutor
    // ---------------------------------------------------------------

    namespace detail
    {
        struct SerialState
        {
            std::mutex mutex;
            std::deque<Callback> queue;
            bool running = false; // a drain task is queued on / running in `base`
            std::shared_ptr<IExecutor> base;
            ErrorHandler onError;
        };

        void serialDrain(const std::shared_ptr<SerialState>& state)
        {
            SerialState& s = *state;

            for (;;)
            {
                Callback callback;

                {
                    std::lock_guard lock(s.mutex);

                    if (s.queue.empty())
                    {
                        s.running = false;
                        return;
                    }

                    callback = std::move(s.queue.front());
                    s.queue.pop_front();
                }

                invokeGuarded(callback, s.onError);
            }
        }
    }

    SerialExecutor::SerialExecutor(std::shared_ptr<IExecutor> base, ErrorHandler onError)
        : m_state(std::make_shared<detail::SerialState>())
    {
        m_state->base = std::move(base);
        m_state->onError = std::move(onError);
    }

    SerialExecutor::~SerialExecutor() = default;

    bool SerialExecutor::post(Callback callback)
    {
        if (!callback)
            return true;

        detail::SerialState& s = *m_state;

        {
            std::lock_guard lock(s.mutex);
            s.queue.push_back(std::move(callback));

            if (s.running)
                return true;

            s.running = true;
        }

        // The drain task keeps the state alive, so this executor object
        // may be destroyed while callbacks are still pending.
        auto state = m_state;

        if (s.base && s.base->post([state] { detail::serialDrain(state); }))
            return true;

        std::lock_guard lock(s.mutex);
        s.queue.clear();
        s.running = false;
        return false;
    }
}
