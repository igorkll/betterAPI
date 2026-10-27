#pragma once

// Two libraries, two export macros:
//
//  - DSBOX_SCHEDULER_API        the scheduler proper (DSBoxScheduler):
//                               Scheduler, executors, TimerHandle, ...
//                               Depends on nothing but the standard
//                               library, so any module -- including ones
//                               that know nothing about dsbox_core, like
//                               dsbox_ipc -- can use it.
//
//  - DSBOX_SCHEDULER_MODULE_API the optional dsbox module wrapper
//                               (DSBoxSchedulerModule): SchedulerModule.
//                               The only part that needs dsbox_core.

#if defined(_WIN32) || defined(_WIN64)
    #if defined(DSBOX_SCHEDULER_EXPORTING)
        #define DSBOX_SCHEDULER_API __declspec(dllexport)
    #else
        #define DSBOX_SCHEDULER_API __declspec(dllimport)
    #endif

    #if defined(DSBOX_SCHEDULER_MODULE_EXPORTING)
        #define DSBOX_SCHEDULER_MODULE_API __declspec(dllexport)
    #else
        #define DSBOX_SCHEDULER_MODULE_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define DSBOX_SCHEDULER_API __attribute__((visibility("default")))
    #define DSBOX_SCHEDULER_MODULE_API __attribute__((visibility("default")))
#else
    #define DSBOX_SCHEDULER_API
    #define DSBOX_SCHEDULER_MODULE_API
#endif
