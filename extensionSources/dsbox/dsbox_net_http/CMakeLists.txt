add_library(DSBoxNetHttp SHARED
    src/http_module.cpp
    src/http_protocol_handler.cpp
)
add_library(DSBox::NetHttp ALIAS DSBoxNetHttp)

target_include_directories(DSBoxNetHttp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetHttp PRIVATE DSBOX_NET_HTTP_EXPORTING)

# Net and NetSocket are public: HttpProtocolHandler implements
# dsbox_net::IProtocolHandler and its constructor takes a resolver
# returning dsbox_net_socket::IConnector. Config is private, exactly like
# every other net module's use of it (HttpModule::onInit() reads [net.http]
# overrides; that type never appears in a public header here).
target_link_libraries(DSBoxNetHttp
    PUBLIC DSBox::Net DSBox::NetSocket
    PRIVATE DSBox::Config
)

if(NOT WIN32 AND NOT MSVC)
    target_link_libraries(DSBoxNetHttp PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
