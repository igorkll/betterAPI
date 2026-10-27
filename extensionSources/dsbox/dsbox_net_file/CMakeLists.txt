add_library(DSBoxNetFile SHARED
    src/file_module.cpp
    src/file_protocol_handler.cpp
)
add_library(DSBox::NetFile ALIAS DSBoxNetFile)

target_include_directories(DSBoxNetFile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetFile PRIVATE DSBOX_NET_FILE_EXPORTING)

# Net is a public dependency: the public API (FileProtocolHandler,
# FileModule) is built entirely out of dsbox_net types (IProtocolHandler,
# Request/Response, NetworkManager, ...). Core comes along transitively
# through Net (dsbox_net::NetworkManager already links it PUBLIC), so it
# isn't listed again here.
# Config is a private implementation detail, exactly like
# dsbox_net::NetModule's own use of it: FileModule::onInit() reads
# `path_style` from a dsbox_config::PersistentConfigStore if the host
# happens to have provide()'d one, but that type never appears in a
# public dsbox_net_file header.
target_link_libraries(DSBoxNetFile
    PUBLIC DSBox::Net
    PRIVATE DSBox::Config
)

if(NOT MSVC)
    target_link_libraries(DSBoxNetFile PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
