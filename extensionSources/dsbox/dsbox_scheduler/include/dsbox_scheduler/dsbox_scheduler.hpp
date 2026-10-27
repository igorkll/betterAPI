#pragma once

// Convenience umbrella header for dsbox_scheduler (the scheduler library).
//
// Deliberately does NOT include <dsbox_scheduler/scheduler_module.hpp>:
// that header needs dsbox_core, and this one must stay usable by code
// that does not link it. Include the module header explicitly, and link
// DSBox::SchedulerModule, if you want SchedulerModule.

#include <dsbox_scheduler/clock.hpp>
#include <dsbox_scheduler/coroutines.hpp>
#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>
#include <dsbox_scheduler/task.hpp>
#include <dsbox_scheduler/timer_queue.hpp>
#include <dsbox_scheduler/types.hpp>
