#pragma once

#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>
#include <dsbox_scheduler/task.hpp>

#include <atomic>
#include <coroutine>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

// Coroutine glue: spawn() to start a Task, resumeOn() to hop executors,
// fromCallback() to co_await a callback-style API. Task itself is in
// task.hpp; co_await scheduler.sleepFor(...) is in scheduler.hpp.

namespace dsbox_scheduler
{
    // ------------------------------------------------------------------
    // spawn
    // ------------------------------------------------------------------

    namespace detail
    {
        struct DetachedTask
        {
            struct promise_type
            {
                DetachedTask get_return_object() noexcept
                {
                    return {};
                }

                std::suspend_never initial_suspend() const noexcept
                {
                    return {};
                }

                std::suspend_never final_suspend() const noexcept
                {
                    return {};
                }

                void return_void() const noexcept
                {
                }

                void unhandled_exception() const noexcept
                {
                }
            };
        };

        template<typename T>
        DetachedTask runSpawned(Task<T> task, std::promise<T> promise)
        {
            try
            {
                if constexpr (std::is_void_v<T>)
                {
                    co_await std::move(task);
                    promise.set_value();
                }
                else
                {
                    promise.set_value(co_await std::move(task));
                }
            }
            catch (...)
            {
                try
                {
                    promise.set_exception(std::current_exception());
                }
                catch (...)
                {
                }
            }
        }
    }

    // Starts `task` on `executor` and returns a future for its result (or
    // the exception it ended with). Fire-and-forget callers may simply
    // drop the future: unlike std::async's, it does not block on
    // destruction.
    //
    // If the executor rejects the start, the future holds a
    // std::runtime_error. If the task is discarded without ever running
    // (the executor drops queued callbacks when destroyed), the future
    // throws std::future_error(broken_promise).
    template<typename T>
    std::future<T> spawn(std::shared_ptr<IExecutor> executor, Task<T> task)
    {
        struct Holder
        {
            Task<T> task;
            std::promise<T> promise;
        };

        auto holder = std::make_shared<Holder>(Holder{ std::move(task), {} });
        std::future<T> future = holder->promise.get_future();

        if (!executor || !executor->post([holder] { detail::runSpawned<T>(std::move(holder->task), std::move(holder->promise)); }))
        {
            holder->promise.set_exception(
                std::make_exception_ptr(std::runtime_error("dsbox_scheduler: spawn rejected (no executor, or it is shut down)")));
        }

        return future;
    }

    // spawn() on the scheduler's default executor.
    template<typename T>
    std::future<T> spawn(Scheduler& scheduler, Task<T> task)
    {
        return spawn<T>(scheduler.executor(), std::move(task));
    }

    // ------------------------------------------------------------------
    // resumeOn
    // ------------------------------------------------------------------

    class ResumeOnAwaiter
    {
    public:
        explicit ResumeOnAwaiter(std::shared_ptr<IExecutor> executor) noexcept
            : m_executor(std::move(executor))
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            // Only touch `this` on the failure path: on success the
            // coroutine may already be running elsewhere.
            if (m_executor && m_executor->post([handle] { handle.resume(); }))
                return true;

            m_accepted = false;
            return false; // do not suspend; continue right here
        }

        // true: now running on the executor. false: the executor
        // rejected the hop (shut down), so execution simply continued on
        // the current thread.
        bool await_resume() const noexcept
        {
            return m_accepted;
        }

    private:
        std::shared_ptr<IExecutor> m_executor;
        bool m_accepted = true;
    };

    // co_await resumeOn(loop);   // continues on `loop`'s thread(s)
    [[nodiscard]] inline ResumeOnAwaiter resumeOn(std::shared_ptr<IExecutor> executor)
    {
        return ResumeOnAwaiter(std::move(executor));
    }

    // Non-owning: `executor` must outlive the suspension.
    [[nodiscard]] inline ResumeOnAwaiter resumeOn(IExecutor& executor)
    {
        return ResumeOnAwaiter(nonOwning(executor));
    }

    // ------------------------------------------------------------------
    // fromCallback
    // ------------------------------------------------------------------

    namespace detail
    {
        template<typename T>
        struct CallbackState
        {
            std::optional<T> value;
            std::coroutine_handle<> handle;
            std::shared_ptr<IExecutor> resumeExecutor;

            // Set by whichever of "await_suspend has returned" and "the
            // callback has been called" happens SECOND -- that side is
            // the one that resumes (or, for await_suspend, declines to
            // suspend). Makes a synchronous callback safe.
            std::atomic<bool> arrived{ false };
            std::atomic<bool> completed{ false };
        };

        template<typename T>
        void completeCallback(CallbackState<T>& state, T value)
        {
            if (state.completed.exchange(true))
                return; // a callback that fires twice is ignored

            state.value.emplace(std::move(value));

            if (!state.arrived.exchange(true, std::memory_order_acq_rel))
                return; // await_suspend has not finished; it will see the value

            const std::coroutine_handle<> handle = state.handle;

            if (state.resumeExecutor && state.resumeExecutor->post([handle] { handle.resume(); }))
                return;

            handle.resume();
        }
    }

    template<typename T, typename Starter>
    class CallbackAwaiter
    {
    public:
        CallbackAwaiter(Starter starter, std::shared_ptr<IExecutor> resumeExecutor)
            : m_starter(std::move(starter))
            , m_state(std::make_shared<detail::CallbackState<T>>())
        {
            m_state->resumeExecutor = std::move(resumeExecutor);
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto state = m_state; // `this` may be gone once the coroutine resumes
            state->handle = handle;

            m_starter(std::function<void(T)>([state](T value) { detail::completeCallback<T>(*state, std::move(value)); }));

            // Second to arrive? Then the callback already ran: do not suspend.
            return !state->arrived.exchange(true, std::memory_order_acq_rel);
        }

        T await_resume()
        {
            return std::move(*m_state->value);
        }

    private:
        Starter m_starter;
        std::shared_ptr<detail::CallbackState<T>> m_state;
    };

    // Turns a callback-style API into something co_await-able:
    //
    //   FetchResult r = co_await fromCallback<FetchResult>(
    //       [&](std::function<void(FetchResult)> done)
    //       {
    //           net.fetch(request, std::move(done));
    //       });
    //
    // `starter` is called (on the awaiting thread) with the completion
    // callback; it must arrange for that to be called exactly once with
    // the result -- from any thread, or synchronously before starter
    // returns. Further calls are ignored. The coroutine resumes on the
    // thread that calls the completion callback, or on `resumeExecutor`
    // if given.
    //
    // If `starter` throws, the exception is thrown from the co_await.
    // If the completion callback is never called the coroutine never
    // resumes -- that is the callee's contract to keep. T must not be
    // void (use bool or std::monostate for "just tell me it is done").
    template<typename T, typename Starter>
    [[nodiscard]] auto fromCallback(Starter&& starter, std::shared_ptr<IExecutor> resumeExecutor = {})
    {
        static_assert(!std::is_void_v<T>, "fromCallback<T>: T must not be void");

        return CallbackAwaiter<T, std::decay_t<Starter>>(std::forward<Starter>(starter), std::move(resumeExecutor));
    }
}
