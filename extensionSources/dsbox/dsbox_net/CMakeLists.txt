add_library(DSBoxNet SHARED
    src/body.cpp
    src/error.cpp
    src/net_module.cpp
    src/network_manager.cpp
    src/url.cpp
)
add_library(DSBox::Net ALIAS DSBoxNet)

target_include_directories(DSBoxNet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNet PRIVATE DSBOX_NET_EXPORTING)

# Core is a public dependency: NetworkManager's scheme registry is a
# dsbox::CommunicationManager and its public API takes dsbox::IModule*/
# dsbox::Context& (see network_manager.hpp/net_module.hpp), so callers
# need Core's headers regardless.
# Scheduler is public too: NetworkManager::useScheduler() takes a
# dsbox_scheduler::Scheduler, which request timeouts run on (see
# network_manager.hpp). It is the scheduler LIBRARY only -- no module.
# Config is a private implementation detail: NetModule::onInit() reads
# NetOptions overrides from a dsbox_config::PersistentConfigStore *if*
# the host happens to have provide()'d one into the Context, but that
# type never appears in a public dsbox_net header -- consumers of
# DSBoxNet don't need to know Config exists unless they want to set
# those overrides themselves (see net_module.hpp's own docs for the
# recognized [net] keys).
target_link_libraries(DSBoxNet
    PUBLIC DSBox::Core DSBox::Scheduler
    PRIVATE DSBox::Config
)

if(NOT MSVC)
    target_link_libraries(DSBoxNet PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
