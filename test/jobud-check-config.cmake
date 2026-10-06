if(NOT DEFINED JOBUD_EXECUTABLE)
    message(FATAL_ERROR "jobud is required")
endif()

# A root CI process cannot trust runner-owned checkout parents. Atomically create
# this process's private leaf beneath the shared temporary directory instead.
execute_process(
    COMMAND mktemp -d /tmp/jobud-check-config.XXXXXX
    RESULT_VARIABLE temporary_status
    OUTPUT_VARIABLE test_directory
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE temporary_error
)
if(NOT temporary_status EQUAL 0 OR test_directory STREQUAL "")
    message(FATAL_ERROR "private config directory creation failed: ${temporary_status} ${temporary_error}")
endif()
file(CHMOD "${test_directory}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(config_file "${test_directory}/jobud.ini")
set(database_file "${test_directory}/unused.sqlite")
set(socket_file "${test_directory}/unused.sock")
file(WRITE "${config_file}"
    "database.path = ${database_file}\nsocket.path = ${socket_file}\ncli.concurrency = 3\n")

execute_process(
    COMMAND "${JOBUD_EXECUTABLE}" --check-config --config jobud.ini
    WORKING_DIRECTORY "${test_directory}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

# Inspect before cleanup so a failed check still reports any unintended resources.
set(created_resources FALSE)
if(EXISTS "${database_file}" OR EXISTS "${socket_file}")
    set(created_resources TRUE)
endif()
file(REMOVE_RECURSE "${test_directory}")
if(EXISTS "${test_directory}")
    message(FATAL_ERROR "private config directory cleanup failed")
endif()

if(NOT status EQUAL 0 OR NOT output MATCHES "Configuration valid:" OR NOT error STREQUAL "")
    message(FATAL_ERROR "local config check failed: ${status} ${error}")
endif()
if(created_resources)
    message(FATAL_ERROR "local config check created daemon resources")
endif()
