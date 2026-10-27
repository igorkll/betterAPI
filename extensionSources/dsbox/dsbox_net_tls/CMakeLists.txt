add_library(DSBoxNetTls SHARED
    src/tls_module.cpp
    src/trust_manager.cpp
)
add_library(DSBox::NetTls ALIAS DSBoxNetTls)

target_include_directories(DSBoxNetTls PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetTls PRIVATE DSBOX_NET_TLS_EXPORTING)

# Core only: this module is the trust-aggregation layer (Certificate,
# ITrustProvider, TrustManager) plus the IModule that publishes a
# TrustManager into Context. It deliberately does not link OpenSSL (or
# any TLS library) itself -- turning the aggregated certificates into
# something a TLS library can verify against is a separate, later layer
# built on top of this one; see this module's own docs.
target_link_libraries(DSBoxNetTls PUBLIC DSBox::Core)

if(NOT MSVC)
    target_link_libraries(DSBoxNetTls PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
