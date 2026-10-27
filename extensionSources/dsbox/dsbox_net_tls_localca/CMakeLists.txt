add_library(DSBoxNetTlsLocalCa SHARED
    src/localca_module.cpp
    src/localca_trust_provider.cpp
)
add_library(DSBox::NetTlsLocalCa ALIAS DSBoxNetTlsLocalCa)

target_include_directories(DSBoxNetTlsLocalCa PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetTlsLocalCa PRIVATE DSBOX_NET_TLS_LOCALCA_EXPORTING)

# NetTls is a public dependency: the public API is built entirely out of
# dsbox_net_tls types (ITrustProvider, TrustManager). Core comes along
# transitively through it (dsbox_net_tls::TrustManager already links it
# PUBLIC), so it isn't listed again here. No dsbox_config dependency --
# unlike dsbox_net_tls_systemca's UseSystemCA, this module reads no
# configuration at all (see LocalCaModule's own docs).
target_link_libraries(DSBoxNetTlsLocalCa PUBLIC DSBox::NetTls)

if(NOT MSVC)
    target_link_libraries(DSBoxNetTlsLocalCa PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
