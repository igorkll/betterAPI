#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace dsbox_scheduler
{
    template<typename T = void>
    class Task;

    namespace detail
    {
        struct TaskPromiseBase
        {
            std::coroutine_handle<> continuation;

            struct FinalAwaiter
            {
                bool await_ready() const noexcept
                {
                    return false;
                }

                // Symmetric transfer: hand control straight to whoever
                // awaited this task, without growing the stack.
                template<typename Promise>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept
                {
                    const auto next = handle.promise().continuation;
                    return next ? next : std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };

            // Lazy: nothing runs until the task is awaited (or spawned).
            std::suspend_always initial_suspend() const noexcept
            {
                return {};
            }

            FinalAwaiter final_suspend() const noexcept
            {
                return {};
            }
        };

        template<typename T>
        struct TaskPromise : TaskPromiseBase
        {
            std::optional<T> value;
            std::exception_ptr error;

            Task<T> get_return_object() noexcept;

            template<typename U = T>
            void return_value(U&& result)
            {
                value.emplace(std::forward<U>(result));
            }

            void unhandled_exception() noexcept
            {
                error = std::current_exception();
            }

            T take()
            {
                if (error)
                    std::rethrow_exception(error);

                return std::move(*value);
            }
        };

        template<>
        struct TaskPromise<void> : TaskPromiseBase
        {
            std::exception_ptr error;

            Task<void> get_return_object() noexcept;

            void return_void() noexcept
            {
            }

            void unhandled_exception() noexcept
            {
                error = std::current_exception();
            }

            void take()
            {
                if (error)
                    std::rethrow_exception(error);
            }
        };
    }

    // A lazily started coroutine producing a T (or nothing).
    //
    //   Task<int> compute(Scheduler& s)
    //   {
    //       co_await s.sleepFor(10ms);
    //       co_return 42;
    //   }
    //
    //   Task<void> caller(Scheduler& s)
    //   {
    //       int v = co_await compute(s);   // runs compute() to completion
    //   }
    //
    // Nothing happens until the Task is co_await-ed from another
    // coroutine or handed to spawn() (coroutines.hpp). Awaiting it resumes
    // the awaiter on whatever thread the task finished on -- use
    // resumeOn() to hop to a specific executor afterwards.
    //
    // An exception thrown inside the coroutine is rethrown from the
    // co_await. The Task owns the coroutine frame: destroying a Task that
    // was never awaited/spawned just destroys the frame.
    //
    // Move-only; co_await it as an rvalue.
    template<typename T>
    class [[nodiscard]] Task
    {
        static_assert(!std::is_reference_v<T>, "Task<T&> is not supported");

    public:
        using promise_type = detail::TaskPromise<T>;

        Task() noexcept = default;

        Task(Task&& other) noexcept
            : m_handle(std::exchange(other.m_handle, {}))
        {
        }

        Task& operator=(Task&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, {});
            }

            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        ~Task()
        {
            reset();
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(m_handle);
        }

        bool done() const noexcept
        {
            return m_handle && m_handle.done();
        }

        auto operator co_await() && noexcept
        {
            struct Awaiter
            {
                std::coroutine_handle<promise_type> handle;

                bool await_ready() const noexcept
                {
                    return false;
                }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
                {
                    handle.promise().continuation = caller;
                    return handle;
                }

                T await_resume()
                {
                    return handle.promise().take();
                }
            };

            return Awaiter{ m_handle };
        }

    private:
        friend struct detail::TaskPromise<T>;

        explicit Task(std::coroutine_handle<promise_type> handle) noexcept
            : m_handle(handle)
        {
        }

        void reset() noexcept
        {
            if (m_handle)
            {
                m_handle.destroy();
                m_handle = nullptr;
            }
        }

        std::coroutine_handle<promise_type> m_handle;
    };

    namespace detail
    {
        template<typename T>
        Task<T> TaskPromise<T>::get_return_object() noexcept
        {
            return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
        }

        inline Task<void> TaskPromise<void>::get_return_object() noexcept
        {
            return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
        }
    }
}
