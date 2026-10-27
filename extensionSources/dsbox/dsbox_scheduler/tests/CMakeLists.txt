add_executable(DSBoxSchedulerTests
    main.cpp
    timer_queue_tests.cpp
    executor_tests.cpp
    scheduler_tests.cpp
    coroutine_tests.cpp
)

target_link_libraries(DSBoxSchedulerTests PRIVATE DSBox::Scheduler)

if(NOT MSVC)
    target_link_libraries(DSBoxSchedulerTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxSchedulerTests executables/test)
dsbox_copy_runtime_dlls(DSBoxSchedulerTests)

include(CTest)
add_test(NAME DSBoxSchedulerTests COMMAND DSBoxSchedulerTests)
