add_executable(DSBoxLauncher src/main.cpp)

target_include_directories(DSBoxLauncher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DSBoxLauncher PRIVATE
    DSBox::Core
    DSBox::IO
    DSBox::Cli
    DSBox::Config
)

set_target_properties(DSBoxLauncher PROPERTIES
    ENABLE_EXPORTS TRUE
    OUTPUT_NAME "dsbox"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
