cmake_minimum_required(VERSION 3.20)
execute_process(COMMAND "${php_binary}" -n "-dextension=${module}" -m
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "\njobu\n" OR NOT error STREQUAL "")
    message(FATAL_ERROR "JobU was not cleanly listed by php -m: ${output}\n${error}")
endif()
execute_process(COMMAND "${php_binary}" -n "-dextension=${module}" --ri jobu
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "JobU module information failed: ${output}\n${error}")
endif()
foreach(expected "JobU support => enabled" "JobU extension version => ${version}" "Thread mode => NTS")
    string(FIND "${output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing module information '${expected}': ${output}")
    endif()
endforeach()

# Linking native clients instantiates standard-library templates with explicit
# visibility. Verify the actual shared object, not just compiler visibility flags.
if(system STREQUAL "Linux")
    execute_process(COMMAND "${nm}" -D --defined-only --format=posix "${module}"
        RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE error TIMEOUT 10)
    string(STRIP "${symbols}" symbols)
    string(REPLACE "\n" ";" exported "${symbols}")
    list(LENGTH exported count)
    if(NOT "${result}" STREQUAL "0" OR NOT error STREQUAL "" OR
       NOT count EQUAL 1 OR NOT symbols MATCHES "^get_module T ")
        message(FATAL_ERROR "Unexpected JobU module exports: ${symbols}\n${error}")
    endif()
endif()
