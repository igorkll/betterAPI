add_library(DSBoxAndroidAdb SHARED
    src/adb_module.cpp
    src/adb_device.cpp
    src/adb_shell.cpp
    src/adb_packages.cpp
    src/command_executor.cpp
)
add_library(DSBox::AndroidAdb ALIAS DSBoxAndroidAdb)

target_include_directories(DSBoxAndroidAdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxAndroidAdb PRIVATE DSBOX_ANDROID_ADB_EXPORTING)

# Process spawns and reads `adb`'s stdout/stderr. Config is private: only
# adb_module.cpp knows about dsbox_config::PersistentConfigStore, exactly
# like dsbox_net_socket::SocketModule's use of it -- that type never
# appears in a public dsbox_android_adb header. Scheduler is private too
# and looked up optionally through the Context (see SchedulerModule's own
# docs on why it is never a hard dependency): dsbox_android_adb does not
# even require it to be loaded, so linking it PRIVATEly here is enough to
# pull in the type without exposing it publicly.
target_link_libraries(DSBoxAndroidAdb
    PUBLIC DSBox::Core DSBox::Process
    PRIVATE DSBox::Config DSBox::Scheduler
)

if(NOT MSVC)
    target_link_libraries(DSBoxAndroidAdb PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
