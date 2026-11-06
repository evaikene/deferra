cmake_minimum_required(VERSION 3.20)
# Each child exercises a real module/request lifetime. Reused interpreter/FPM
# request ownership belongs to the session stage, not this stateless skeleton.
foreach(cycle RANGE 1 3)
    execute_process(COMMAND "${php_binary}" -n "-dextension=${module}"
        "${CMAKE_CURRENT_LIST_DIR}/exceptions.php"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 8)
    if(NOT "${result}" STREQUAL "0" OR NOT output STREQUAL "exceptions ok\n" OR NOT error STREQUAL "")
        message(FATAL_ERROR "Load/shutdown cycle ${cycle} failed: ${output}\n${error}")
    endif()
endforeach()
