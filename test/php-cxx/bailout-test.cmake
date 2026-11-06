# A fatal diagnostic alone is not success: require normal child exit and its
# post-shutdown marker. A crash, timeout, or missing bailout must fail CTest.
execute_process(COMMAND "${executable}" "${scenario}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
if(NOT result STREQUAL "0" OR
   NOT output MATCHES "php-cxx isolated bailout observed; shutdown completed")
    message(FATAL_ERROR "Isolated ${scenario} failed (${result}):\n${output}\n${error}")
endif()
if(scenario STREQUAL "fatal" AND NOT "${output}${error}" MATCHES "php-cxx deliberate frameless error")
    message(FATAL_ERROR "Missing expected fatal diagnostic:\n${output}\n${error}")
endif()
