if(NOT DSBOX_RUNTIME_DLLS)
    return()
endif()

foreach(_dsbox_runtime_dll IN LISTS DSBOX_RUNTIME_DLLS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_dsbox_runtime_dll}"
            "${DSBOX_RUNTIME_DLLS_DEST}"
        RESULT_VARIABLE _dsbox_copy_result
    )
    if(NOT "${_dsbox_copy_result}" STREQUAL "0")
        message(FATAL_ERROR
            "Failed to copy runtime DLL '${_dsbox_runtime_dll}' to '${DSBOX_RUNTIME_DLLS_DEST}' (exit code ${_dsbox_copy_result})")
    endif()
endforeach()
