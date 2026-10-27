#pragma once

#include <chrono>
#include <exception>
#include <functional>

namespace dsbox_scheduler
{
    // The time base for everything in this library. steady_clock (not
    // system_clock): timers must never fire early or late because
    // somebody changed the wall clock. Tests substitute a ManualClock
    // (see clock.hpp) that produces TimePoints of this same type, so
    // no code here needs to be a template over the clock.
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    // A unit of work handed to an executor or a timer. Must be
    // copyable (std::function) -- to carry a move-only object into one,
    // hold it in a std::shared_ptr captured by the lambda.
    using Callback = std::function<void()>;

    // Receives exceptions that escaped a Callback run by an executor or
    // a timer. Without one they are swallowed: a throwing callback must
    // never take down a worker thread (or the whole process, via
    // std::terminate). Must not throw itself (if it does, that is
    // swallowed too). May be called from any thread.
    using ErrorHandler = std::function<void(std::exception_ptr)>;
}
