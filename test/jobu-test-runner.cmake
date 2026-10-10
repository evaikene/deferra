cmake_minimum_required(VERSION 3.20)

foreach(argument test_name test_script evidence_directory)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "JobU test runner requires ${argument}")
    endif()
endforeach()
if(NOT test_name MATCHES "^jobu-[a-z-]+-test$")
    message(FATAL_ERROR "Invalid JobU test name: ${test_name}")
endif()

# Keep only the latest run's small diagnostics on disk. The private /tmp leaf
# remains outside the checkout and build tree for SDK isolation and path trust.
file(REMOVE_RECURSE "${evidence_directory}")
string(REGEX REPLACE "-test$" "" fixture_name "${test_name}")
execute_process(COMMAND mktemp -d "/tmp/${fixture_name}.XXXXXX"
    RESULT_VARIABLE result OUTPUT_VARIABLE fixture OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result STREQUAL "0" OR fixture STREQUAL "")
    message(FATAL_ERROR "Private ${test_name} fixture creation failed")
endif()
set(test_logs "${fixture}/logs")
file(MAKE_DIRECTORY "${test_logs}")

# A worker's FATAL_ERROR exits only the child CMake process. This owner can
# preserve its logs and clean all scratch data on both success and normal failure.
# Killing this runner itself (including a CTest timeout) can still leave scratch.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-Dfixture=${fixture}" "-Dtest_logs=${test_logs}"
    "-Dtest_configuration=${test_configuration}" -P "${test_script}"
    RESULT_VARIABLE test_result
    OUTPUT_FILE "${test_logs}/runner.log" ERROR_FILE "${test_logs}/runner.log")
file(READ "${test_logs}/runner.log" test_output)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${test_logs}" "${evidence_directory}"
    RESULT_VARIABLE save_result)
file(REMOVE_RECURSE "${fixture}")

message("${test_output}")
message(STATUS "${test_name} logs saved in ${evidence_directory}")
if(EXISTS "${fixture}")
    message(FATAL_ERROR "Scratch cleanup failed: ${fixture}")
endif()
if(NOT save_result STREQUAL "0")
    message(FATAL_ERROR "Could not preserve ${test_name} logs (${save_result})")
endif()
if(NOT test_result STREQUAL "0")
    message(FATAL_ERROR "${test_name} failed (${test_result}); logs: ${evidence_directory}")
endif()
