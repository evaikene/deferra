find_package(Catch2 CONFIG REQUIRED)

enable_testing()

# Object test
set(TARGET "object-test")
add_executable(${TARGET} object-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU attribute test
set(TARGET "attribute-test")
add_executable(${TARGET} attribute-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Test support test
set(TARGET "test-support-test")
add_executable(${TARGET}
    test-support-test.cpp
    support/temporary_directory.cpp
)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic database API test
set(TARGET "db-api-test")
add_executable(${TARGET}
    db-api-test.cpp
    support/fake_database_driver.cpp
)
target_link_libraries(${TARGET} PRIVATE db Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic database query test
set(TARGET "db-query-test")
add_executable(${TARGET}
    db-query-test.cpp
    support/fake_database_driver.cpp
)
target_link_libraries(${TARGET} PRIVATE db Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic database transaction test
set(TARGET "db-transaction-test")
add_executable(${TARGET}
    db-transaction-test.cpp
    support/fake_database_driver.cpp
)
target_link_libraries(${TARGET} PRIVATE db Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

if(TARGET db-sqlite)
    # SQLite database integration test
    set(TARGET "db-sqlite-test")
    add_executable(${TARGET}
        db-sqlite-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE db-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # SQLite public header boundary test
    set(TARGET "db-sqlite-public-headers-test")
    add_executable(${TARGET} db-sqlite-public-headers-test.cpp)
    target_link_libraries(${TARGET} PRIVATE db-sqlite)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
endif()

# Public header boundary test
set(TARGET "public-headers-test")
add_executable(${TARGET} public-headers-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# RPC public header boundary test
set(TARGET "rpc-public-headers-test")
add_executable(${TARGET} rpc-public-headers-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JSON codec test
set(TARGET "json-test")
add_executable(${TARGET} json-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# RPC stream framing test
set(TARGET "rpc-framing-test")
add_executable(${TARGET} rpc-framing-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Byte buffer test
set(TARGET "byte-buffer-test")
add_executable(${TARGET} byte-buffer-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Time source test
set(TARGET "time-source-test")
add_executable(${TARGET} time-source-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# UUID test
set(TARGET "uuid-test")
add_executable(${TARGET} uuid-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Result test
set(TARGET "result-test")
add_executable(${TARGET} result-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU executable smoke tests
add_test(NAME jobud-version COMMAND $<TARGET_FILE:jobud> --version)
add_test(NAME jobud-not-implemented COMMAND $<TARGET_FILE:jobud>)
set_tests_properties(jobud-not-implemented PROPERTIES
    WILL_FAIL TRUE
)

add_test(NAME jobuctl-version COMMAND $<TARGET_FILE:jobuctl> --version)
add_test(NAME jobuctl-not-implemented COMMAND $<TARGET_FILE:jobuctl>)
set_tests_properties(jobuctl-not-implemented PROPERTIES
    WILL_FAIL TRUE
)

# Signal test
set(TARGET "signal-test")
add_executable(${TARGET} signal-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Application test
set(TARGET "application-test")
add_executable(${TARGET} application-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Command line parser test
set(TARGET "command-line-parser-test")
add_executable(${TARGET} command-line-parser-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Threaded event loop test
set(TARGET "event-thread-test")
add_executable(${TARGET} event-thread-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Shared data test
set(TARGET "shared-data-test")
add_executable(${TARGET} shared-data-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Enum bitmask test
set(TARGET "enum-bitmask-test")
add_executable(${TARGET} enum-bitmask-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Event loop test
set(TARGET "event-loop-test")
add_executable(${TARGET} event-loop-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Event test
set(TARGET "event-test")
add_executable(${TARGET} event-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Timer test
set(TARGET "timer-test")
add_executable(${TARGET} timer-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Logging test
set(TARGET "logging-test")
add_executable(${TARGET} logging-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# INI file test
set(TARGET "ini-file-test")
add_executable(${TARGET} ini-file-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Utils test
set(TARGET "utils-test")
add_executable(${TARGET} utils-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# IODevice test
set(TARGET "io-device-test")
add_executable(${TARGET} io-device-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# File test
set(TARGET "file-test")
add_executable(${TARGET} file-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# TCP socket test
set(TARGET "tcp-socket-test")
add_executable(${TARGET} tcp-socket-test.cpp)
target_link_libraries(${TARGET} PRIVATE net Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
