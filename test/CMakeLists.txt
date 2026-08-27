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

# JobU standard attribute registry, materialization, and public JSON test
set(TARGET "attribute-materialization-test")
add_executable(${TARGET} attribute-materialization-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU retry policy calculation test
set(TARGET "retry-policy-test")
add_executable(${TARGET} retry-policy-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU UTC timestamp test
set(TARGET "utc-timestamp-test")
add_executable(${TARGET} utc-timestamp-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU cron expression and local calendar search test
set(TARGET "cron-expression-test")
add_executable(${TARGET} cron-expression-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU checked TZif and POSIX-footer reader test
set(TARGET "cron-timezone-test")
add_executable(${TARGET}
    cron-timezone-test.cpp
    support/temporary_directory.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU domain storage adapter test
set(TARGET "domain-storage-test")
add_executable(${TARGET} domain-storage-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU system information test
set(TARGET "system-info-test")
add_executable(${TARGET} system-info-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU transport-independent system information RPC test
set(TARGET "system-info-rpc-test")
add_executable(${TARGET}
    system-info-rpc-test.cpp
    support/memory_io_device.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU transport-independent management JSON test
set(TARGET "management-json-test")
add_executable(${TARGET} management-json-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU transport-independent job management JSON test
set(TARGET "job-management-json-test")
add_executable(${TARGET} job-management-json-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Test support test
set(TARGET "test-support-test")
add_executable(${TARGET}
    test-support-test.cpp
    support/fake_attempt_executor.cpp
    support/temporary_directory.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU backend-independent atomic scheduler dispatch test
set(TARGET "scheduler-dispatch-test")
add_executable(${TARGET}
    scheduler-dispatch-test.cpp
    support/fake_attempt_executor.cpp
    support/fake_database_driver.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
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

if(TARGET jobu-sqlite)
    # JobU SQLite-backed transport-independent management RPC test
    set(TARGET "management-rpc-test")
    add_executable(${TARGET}
        management-rpc-test.cpp
        support/memory_io_device.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed idempotency, secret, and retention repository test
    set(TARGET "jobu-idempotency-secret-retention-test")
    add_executable(${TARGET}
        jobu-idempotency-secret-retention-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed run, attempt, and output repository test
    set(TARGET "jobu-run-attempt-repository-test")
    add_executable(${TARGET}
        jobu-run-attempt-repository-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed scheduler read repository test
    set(TARGET "scheduler-repository-test")
    add_executable(${TARGET}
        scheduler-repository-test.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed deterministic scheduler core test
    set(TARGET "scheduler-core-test")
    add_executable(${TARGET}
        scheduler-core-test.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed public scheduler EventLoop adapter test
    set(TARGET "scheduler-event-loop-test")
    add_executable(${TARGET}
        scheduler-event-loop-test.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed deterministic scheduler integration test
    set(TARGET "scheduler-integration-test")
    add_executable(${TARGET}
        scheduler-integration-test.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed queue management service test
    set(TARGET "jobu-queue-management-test")
    add_executable(${TARGET}
        jobu-queue-management-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed job management service test
    set(TARGET "jobu-job-management-test")
    add_executable(${TARGET}
        jobu-job-management-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite-backed recurring create management test
    set(TARGET "jobu-cron-management-test")
    add_executable(${TARGET}
        jobu-cron-management-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite application schema integration test
    set(TARGET "jobu-sqlite-schema-test")
    add_executable(${TARGET}
        jobu-sqlite-schema-test.cpp
        support/fake_database_driver.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # JobU SQLite public header boundary test
    set(TARGET "jobu-sqlite-public-headers-test")
    add_executable(${TARGET} jobu-sqlite-public-headers-test.cpp)
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
endif()

# Public header boundary test
set(TARGET "public-headers-test")
add_executable(${TARGET} public-headers-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU domain public headers must each compile as the first include.
foreach(JOBU_DOMAIN_HEADER IN ITEMS
        attempt
        attempt-executor
        attribute-registry
        cron
        job
        management
        management-json
        management-rpc
        queue
        run
        scheduler
        secret
        system-info-rpc
        utc-timestamp
)
    set(TARGET "jobu-${JOBU_DOMAIN_HEADER}-public-header-test")
    add_executable(${TARGET} "${TARGET}.cpp")
    target_link_libraries(${TARGET} PRIVATE jobu)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
endforeach()

# RPC public header boundary test
set(TARGET "rpc-public-headers-test")
add_executable(${TARGET} rpc-public-headers-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Core JSON public header boundary test
set(TARGET "core-json-public-header-test")
add_executable(${TARGET} core-json-public-header-test.cpp)
target_link_libraries(${TARGET} PRIVATE core)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Net public header boundary test
set(TARGET "net-public-headers-test")
add_executable(${TARGET} net-public-headers-test.cpp)
target_link_libraries(${TARGET} PRIVATE net)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic HTTP client public header must compile as the first include.
set(TARGET "net-http-client-public-header-test")
add_executable(${TARGET} net-http-client-public-header-test.cpp)
target_link_libraries(${TARGET} PRIVATE net)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# System HTTP client public header must compile as the first include.
set(TARGET "net-http-public-header-test")
add_executable(${TARGET} net-http-public-header-test.cpp)
target_link_libraries(${TARGET} PRIVATE net-http)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JSON codec test
set(TARGET "json-test")
add_executable(${TARGET} json-test.cpp)
target_link_libraries(${TARGET} PRIVATE core Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# RPC stream framing test
set(TARGET "rpc-framing-test")
add_executable(${TARGET} rpc-framing-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JSON-RPC protocol test
set(TARGET "rpc-protocol-test")
add_executable(${TARGET} rpc-protocol-test.cpp)
target_link_libraries(${TARGET} PRIVATE rpc Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Transport-independent RPC server test
set(TARGET "rpc-server-test")
add_executable(${TARGET}
    rpc-server-test.cpp
    support/memory_io_device.cpp
)
target_link_libraries(${TARGET} PRIVATE rpc Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Transport-independent RPC client test
set(TARGET "rpc-client-test")
add_executable(${TARGET}
    rpc-client-test.cpp
    support/memory_io_device.cpp
)
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
if(TARGET jobud)
    add_test(NAME jobud-version COMMAND $<TARGET_FILE:jobud> --version)
    add_test(NAME jobud-usage COMMAND $<TARGET_FILE:jobud>)
    add_test(NAME jobud-missing-database COMMAND $<TARGET_FILE:jobud> --socket unused.sock)
    add_test(NAME jobud-missing-socket COMMAND $<TARGET_FILE:jobud> --database unused.sqlite)
    set_tests_properties(jobud-usage jobud-missing-database jobud-missing-socket PROPERTIES
        WILL_FAIL TRUE
    )
endif()

add_test(NAME jobuctl-version COMMAND $<TARGET_FILE:jobuctl> --version)
add_test(NAME jobuctl-usage COMMAND $<TARGET_FILE:jobuctl>)
set_tests_properties(jobuctl-usage PROPERTIES
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

# Kqueue backend transition test
set(TARGET "kqueue-backend-test")
add_executable(${TARGET} kqueue-backend-test.cpp)
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

# Generic asynchronous HTTP client contract and validation test
set(TARGET "http-client-contract-test")
add_executable(${TARGET} http-client-contract-test.cpp)
target_link_libraries(${TARGET} PRIVATE net Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic bounded HTTP capture primitive test
set(TARGET "http-capture-buffer-test")
add_executable(${TARGET} http-capture-buffer-test.cpp)
target_link_libraries(${TARGET} PRIVATE net Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# System HTTP client options and runtime preflight test
set(TARGET "system-http-client-test")
add_executable(${TARGET}
    system-http-client-test.cpp
    support/http_test_server.cpp
    support/temporary_directory.cpp
)
target_link_libraries(${TARGET} PRIVATE net-http Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    # Platform local socket test
    set(TARGET "local-socket-test")
    add_executable(${TARGET}
        local-socket-test.cpp
        support/temporary_directory.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${TARGET} PRIVATE _GNU_SOURCE)
    endif()
    target_link_libraries(${TARGET} PRIVATE net Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()

    # Platform local server test
    set(TARGET "local-server-test")
    add_executable(${TARGET}
        local-server-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE net Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    # Platform local RPC integration test
    set(TARGET "local-rpc-test")
    add_executable(${TARGET}
        local-rpc-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET}
        PRIVATE
            net
            rpc
            Catch2::Catch2WithMain
    )
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND TARGET jobud)
    # Platform foreground daemon database bootstrap and job control executable integration test
    set(TARGET "jobuctl-system-info-test")
    add_executable(${TARGET}
        jobuctl-system-info-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE db-sqlite)
    add_dependencies(${TARGET} jobud jobuctl)
    add_test(NAME ${TARGET}
        COMMAND $<TARGET_FILE:${TARGET}> $<TARGET_FILE:jobud> $<TARGET_FILE:jobuctl> ${PROJECT_VERSION}
    )
    set_tests_properties(${TARGET} PROPERTIES TIMEOUT 40)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND TARGET jobud)
    # Platform queue management executable lifecycle and restart integration test
    set(TARGET "jobuctl-queue-management-test")
    add_executable(${TARGET}
        jobuctl-queue-management-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE core)
    add_dependencies(${TARGET} jobud jobuctl)
    add_test(NAME ${TARGET}
        COMMAND $<TARGET_FILE:${TARGET}> $<TARGET_FILE:jobud> $<TARGET_FILE:jobuctl>
    )
    set_tests_properties(${TARGET} PROPERTIES TIMEOUT 60)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()

    # Platform job management executable lifecycle, restart, and persistence integration test
    set(TARGET "jobuctl-job-management-test")
    add_executable(${TARGET}
        jobuctl-job-management-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE db-sqlite)
    add_dependencies(${TARGET} jobud jobuctl)
    add_test(NAME ${TARGET}
        COMMAND $<TARGET_FILE:${TARGET}> $<TARGET_FILE:jobud> $<TARGET_FILE:jobuctl>
    )
    set_tests_properties(${TARGET} PROPERTIES TIMEOUT 90)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()
