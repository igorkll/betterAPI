add_library(DSBoxNetTlsOpenSsl SHARED
    src/openssl_module.cpp
    src/tls_connector.cpp
)
add_library(DSBox::NetTlsOpenSsl ALIAS DSBoxNetTlsOpenSsl)

target_include_directories(DSBoxNetTlsOpenSsl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxNetTlsOpenSsl PRIVATE DSBOX_NET_TLS_OPENSSL_EXPORTING)

# NetSocket and NetTls are public: TlsConnector is an IConnector and its
# constructor takes a TrustManager. OpenSSL is private -- no OpenSSL type
# appears in any public header here, so users of this library never need
# OpenSSL's headers. Config is private, as everywhere.
#
# find_package(OpenSSL) is done once at the top level, which only adds
# this directory if OpenSSL was found.
target_link_libraries(DSBoxNetTlsOpenSsl
    PUBLIC DSBox::NetSocket DSBox::NetTls
    PRIVATE DSBox::Config OpenSSL::SSL OpenSSL::Crypto
)

if(NOT WIN32 AND NOT MSVC)
    target_link_libraries(DSBoxNetTlsOpenSsl PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
