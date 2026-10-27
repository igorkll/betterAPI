add_executable(DSBoxConfigTests
    main.cpp
    module_config_loader_tests.cpp
    persistent_config_store_tests.cpp
)

target_link_libraries(DSBoxConfigTests PRIVATE
    DSBox::Config
    # DSBox::Ini is otherwise a PRIVATE (hidden) dependency of DSBoxConfig
    # -- linked here only so these tests can white-box-verify
    # PreserveFormatting's output is still valid INI via IniDocument::
    # parse(), not because real consumers of DSBoxConfig should depend on
    # dsbox_ini too.
    DSBox::Ini
)

dsbox_set_output_subdir(DSBoxConfigTests executables/test)
dsbox_copy_runtime_dlls(DSBoxConfigTests)

include(CTest)
add_test(NAME DSBoxConfigTests COMMAND DSBoxConfigTests)
