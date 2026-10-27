add_library(DSBoxNetSocket SHARED
    src/connector_registry.cpp
    src/socket_module.cpp
    src/tcp_connector.cpp
)
add_library(DSBox::NetSocket ALIAS DSBoxNetSocket)

target_include_directories(DSBoxNetSocket PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetSocket PRIVATE DSBOX_NET_SOCKET_EXPORTING)

# Net is a public dependency: IConnection/IConnector report failures as
# dsbox_net::NetException and take a dsbox_net::CancellationToken. Core
# comes along transitively through Net.
# Config is private, exactly like dsbox_net::NetModule's use of it:
# SocketModule::onInit() reads [net.socket] overrides from a
# dsbox_config::PersistentConfigStore if the host provided one, but that
# type never appears in a public dsbox_net_socket header.
target_link_libraries(DSBoxNetSocket
    PUBLIC DSBox::Net
    PRIVATE DSBox::Config
)

if(WIN32)
    target_link_libraries(DSBoxNetSocket PRIVATE ws2_32)
elseif(NOT MSVC)
    target_link_libraries(DSBoxNetSocket PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
