# -- DSBox::Scheduler ---------------------------------------------------------
# Timers, executors and C++20 coroutine support. Depends on nothing but the
# standard library -- in particular NOT on dsbox_core -- so that low-level
# modules (dsbox_ipc, dsbox_net, ...) can use it without dragging the module
# system in. The dsbox module wrapper lives in its own library below.
add_library(DSBoxScheduler SHARED
    src/executors.cpp
    src/scheduler.cpp
)
add_library(DSBox::Scheduler ALIAS DSBoxScheduler)

target_include_directories(DSBoxScheduler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxScheduler PRIVATE DSBOX_SCHEDULER_EXPORTING)

# C++20 coroutines are part of the public API (task.hpp, coroutines.hpp).
# GCC 10 needs the flag spelled out; 11+ and MSVC/Clang do not.
target_compile_features(DSBoxScheduler PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    target_compile_options(DSBoxScheduler PUBLIC -fcoroutines)
endif()

if(NOT MSVC)
    target_link_libraries(DSBoxScheduler PUBLIC Threads::Threads)
endif()

# -- DSBox::SchedulerModule ---------------------------------------------------
# The optional IModule wrapper that publishes one shared Scheduler into the
# module Context. The only part that needs dsbox_core.
#
# Core is public: IModule/Context appear in SchedulerModule's own API.
# Config is a private implementation detail, exactly like NetModule's use of
# it: onInit() reads "[scheduler]" settings if a PersistentConfigStore has
# been provide()'d.
add_library(DSBoxSchedulerModule SHARED
    src/scheduler_module.cpp
)
add_library(DSBox::SchedulerModule ALIAS DSBoxSchedulerModule)

target_include_directories(DSBoxSchedulerModule PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxSchedulerModule PRIVATE DSBOX_SCHEDULER_MODULE_EXPORTING)

target_link_libraries(DSBoxSchedulerModule
    PUBLIC DSBox::Scheduler DSBox::Core
    PRIVATE DSBox::Config
)

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
