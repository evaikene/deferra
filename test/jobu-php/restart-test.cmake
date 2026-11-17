cmake_minimum_required(VERSION 3.20)
# Each child loads and uses the production module with real connected resources,
# then leaves an object graph for request/module shutdown. Reused-runtime request
# isolation is checked separately by the Embed lifecycle test.
foreach(cycle RANGE 1 3)
    execute_process(COMMAND "${driver}" "[connected-cli]"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 35)
    if(NOT "${result}" STREQUAL "0" OR NOT error STREQUAL "" OR
       output MATCHES "Warning:|Fatal error:|memory leaks detected")
        message(FATAL_ERROR "Connected load/shutdown cycle ${cycle} failed: ${output}\n${error}")
    endif()
endforeach()
