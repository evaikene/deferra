if(NOT DEFINED JOBUD_EXECUTABLE OR NOT DEFINED TEST_DIRECTORY)
    message(FATAL_ERROR "jobud and test directory are required")
endif()

file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
set(config_file "${TEST_DIRECTORY}/jobud.ini")
set(database_file "${TEST_DIRECTORY}/unused.sqlite")
set(socket_file "${TEST_DIRECTORY}/unused.sock")
file(WRITE "${config_file}"
    "database.path = ${database_file}\nsocket.path = ${socket_file}\ncli.concurrency = 3\n")

execute_process(
    COMMAND "${JOBUD_EXECUTABLE}" --check-config --config jobud.ini
    WORKING_DIRECTORY "${TEST_DIRECTORY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0 OR NOT output MATCHES "Configuration valid:" OR NOT error STREQUAL "")
    message(FATAL_ERROR "local config check failed: ${status} ${error}")
endif()
if(EXISTS "${database_file}" OR EXISTS "${socket_file}")
    message(FATAL_ERROR "local config check created daemon resources")
endif()
