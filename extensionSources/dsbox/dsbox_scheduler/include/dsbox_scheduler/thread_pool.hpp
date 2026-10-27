#pragma once

// Superseded by <dsbox_scheduler/executor.hpp> (ThreadPoolExecutor).
// The prototype's ThreadPool had no shutdown, no error handling and was
// hidden inside SchedulerModule. This forwarding header only exists so
// stale includes keep compiling -- it is safe to delete the file.

#include <dsbox_scheduler/executor.hpp>
