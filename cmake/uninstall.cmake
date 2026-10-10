# Run by the uninstall target with -DDBOX_BINARY=<prefix>/bin/dbox. The login
# service is disabled first, since it would otherwise keep starting a binary
# that no longer exists.

if(NOT EXISTS "${DBOX_BINARY}")
    message(STATUS "Nothing to uninstall: ${DBOX_BINARY} does not exist")
    return()
endif()

execute_process(
    COMMAND "${DBOX_BINARY}" service disable
    RESULT_VARIABLE disable_result
    OUTPUT_VARIABLE disable_output
    ERROR_VARIABLE disable_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(disable_result EQUAL 0)
    message(STATUS "${disable_output}")
elseif(NOT disable_error MATCHES "no service installed")
    message(FATAL_ERROR "dbox service disable failed, so ${DBOX_BINARY} was kept: ${disable_error}")
endif()

file(REMOVE "${DBOX_BINARY}")
message(STATUS "Removed ${DBOX_BINARY}")
