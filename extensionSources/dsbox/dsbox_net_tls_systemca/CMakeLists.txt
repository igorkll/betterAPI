add_library(DSBoxNetTlsSystemCa SHARED
    src/systemca_module.cpp
    src/systemca_trust_provider.cpp
)
add_library(DSBox::NetTlsSystemCa ALIAS DSBoxNetTlsSystemCa)

target_include_directories(DSBoxNetTlsSystemCa PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetTlsSystemCa PRIVATE DSBOX_NET_TLS_SYSTEMCA_EXPORTING)

# NetTls public (see dsbox_net_tls_localca's own CMakeLists.txt for why);
# Config private -- only this module's use_system_ca toggle needs it,
# never exposed in a public header.
target_link_libraries(DSBoxNetTlsSystemCa
    PUBLIC DSBox::NetTls
    PRIVATE DSBox::Config
)

# Windows reads CryptoAPI's ROOT store; everywhere else the provider
# reads CA files from the filesystem and needs nothing extra.
if(WIN32)
    target_link_libraries(DSBoxNetTlsSystemCa PRIVATE crypt32)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
