add_library(DSBoxIpc SHARED
    src/length_prefixed_frame_codec.cpp
    src/text_line_frame_codec.cpp
    src/ipc_channel.cpp
)
add_library(DSBox::Ipc ALIAS DSBoxIpc)

target_include_directories(DSBoxIpc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxIpc PRIVATE DSBOX_IPC_EXPORTING)

# Request timeouts run on a dsbox_scheduler::Scheduler (see
# IpcChannelOptions::scheduler). Public because that type appears in
# IpcChannelOptions, so a caller who wants to share a scheduler needs its
# headers. DSBox::Scheduler depends on nothing but the standard library,
# so this does not pull dsbox_core into dsbox_ipc.
target_link_libraries(DSBoxIpc PUBLIC DSBox::Scheduler)

if(NOT MSVC)
    target_link_libraries(DSBoxIpc PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
