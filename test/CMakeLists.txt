find_package(Catch2 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

enable_testing()

# Generic forwarding/fault support, independent of SQLite and JobU.
add_executable(fault-database-driver-test
    fault-database-driver-test.cpp support/fault_database_driver.cpp support/fake_database_driver.cpp)
target_link_libraries(fault-database-driver-test PRIVATE db Catch2::Catch2WithMain)
add_test(NAME fault-database-driver-test COMMAND $<TARGET_FILE:fault-database-driver-test>)

foreach(DB_DRIVER_HEADER driver driver_query database)
    set(TARGET "db-${DB_DRIVER_HEADER}-public-header-test")
    add_executable(${TARGET} db-driver-public-header-test.cpp)
    target_compile_definitions(${TARGET} PRIVATE DB_DRIVER_HEADER="${DB_DRIVER_HEADER}.hpp")
    target_link_libraries(${TARGET} PRIVATE db)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
endforeach()

# Process contract and deterministic request preparation (no child launch).
add_executable(process-public-header-test process-public-header-test.cpp)
target_link_libraries(process-public-header-test PRIVATE core)
add_test(NAME process-public-header-test COMMAND $<TARGET_FILE:process-public-header-test>)

add_executable(process-request-test process-request-test.cpp)
target_link_libraries(process-request-test PRIVATE core Catch2::Catch2WithMain)
add_test(NAME process-request-test COMMAND $<TARGET_FILE:process-request-test>)

add_executable(process-test process-test.cpp)
target_link_libraries(process-test PRIVATE core Catch2::Catch2WithMain)
add_test(NAME process-test COMMAND $<TARGET_FILE:process-test>)

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    add_executable(process-test-helper process-test-helper.cpp)
    target_link_libraries(process-test-helper PRIVATE core)
    # Preserve the established Linux target name used by focused CI invocations.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(PROCESS_NATIVE_TEST process-linux-test)
    else()
        set(PROCESS_NATIVE_TEST process-posix-test)
    endif()
    add_executable(${PROCESS_NATIVE_TEST} process-posix-test.cpp support/temporary_directory.cpp)
    target_link_libraries(${PROCESS_NATIVE_TEST} PRIVATE core Catch2::Catch2WithMain)
    target_compile_definitions(${PROCESS_NATIVE_TEST} PRIVATE PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(${PROCESS_NATIVE_TEST} process-test-helper)
    add_test(NAME ${PROCESS_NATIVE_TEST} COMMAND $<TARGET_FILE:${PROCESS_NATIVE_TEST}>)
    set_tests_properties(${PROCESS_NATIVE_TEST} PROPERTIES TIMEOUT 30)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(process-macos-test process-macos-test.cpp)
    target_link_libraries(process-macos-test PRIVATE core Catch2::Catch2WithMain)
    add_test(NAME process-macos-test COMMAND $<TARGET_FILE:process-macos-test>)
    set_tests_properties(process-macos-test PROPERTIES TIMEOUT 30)
endif()

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

# Recovery-specific retry policy (no backend required).
add_executable(recovery-retry-policy-test recovery-retry-policy-test.cpp)
target_link_libraries(recovery-retry-policy-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME recovery-retry-policy-test COMMAND $<TARGET_FILE:recovery-retry-policy-test>)

# JobU operation-context storage failure policy (no backend required).
set(TARGET "storage-failure-test")
add_executable(${TARGET} storage-failure-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Management admission and failure boundaries (no backend required).
add_executable(management-admission-test management-admission-test.cpp support/fake_database_driver.cpp)
target_link_libraries(management-admission-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME management-admission-test COMMAND $<TARGET_FILE:management-admission-test>)

# JobU HTTP retry classification and safe result policy test
set(TARGET "http-retry-test")
add_executable(${TARGET} http-retry-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU HTTP attempt executor and deterministic fake-client test
set(TARGET "http-attempt-executor-test")
add_executable(${TARGET}
    http-attempt-executor-test.cpp
    support/fake_http_client.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu-http Catch2::Catch2WithMain)
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

# History contracts and cursor ownership before repository query integration.
add_executable(history-json-test history-json-test.cpp)
target_link_libraries(history-json-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME history-json-test COMMAND $<TARGET_FILE:history-json-test>)

add_executable(history-cursor-test history-cursor-test.cpp)
target_link_libraries(history-cursor-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME history-cursor-test COMMAND $<TARGET_FILE:history-cursor-test>)

# JobU HTTP payload decoding and validation test
set(TARGET "http-job-payload-test")
add_executable(${TARGET} http-job-payload-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU CLI payload decoding and Process-boundary validation test
set(TARGET "cli-job-payload-test")
add_executable(${TARGET} cli-job-payload-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Shared durable template grammar and concrete-decoder boundary.
add_executable(payload-preparation-test payload-preparation-test.cpp)
target_link_libraries(payload-preparation-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME payload-preparation-test COMMAND $<TARGET_FILE:payload-preparation-test>)

add_executable(payload-template-test payload-template-test.cpp)
target_link_libraries(payload-template-test PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME payload-template-test COMMAND $<TARGET_FILE:payload-template-test>)

# JobU CLI exit-code selector and normalized retry membership test
set(TARGET "cli-exit-policy-test")
add_executable(${TARGET} cli-exit-policy-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU CLI bounded output capture test
set(TARGET "cli-capture-test")
add_executable(${TARGET} cli-capture-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# Generic owned attempt executor routing test
set(TARGET "attempt-executor-group-test")
add_executable(${TARGET}
    attempt-executor-group-test.cpp
    support/fake_attempt_executor.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu Catch2::Catch2WithMain)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU CLI attempt executor and deterministic fake-process test
set(TARGET "cli-attempt-executor-test")
add_executable(${TARGET}
    cli-attempt-executor-test.cpp
    support/fake_process_adapter.cpp
)
target_link_libraries(${TARGET} PRIVATE jobu-cli Catch2::Catch2WithMain)
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    target_sources(${TARGET} PRIVATE support/temporary_directory.cpp)
    target_compile_definitions(${TARGET} PRIVATE PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(${TARGET} process-test-helper)
endif()
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

    add_executable(secret-service-test
        secret-service-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/temporary_directory.cpp)
    target_link_libraries(secret-service-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME secret-service-test COMMAND $<TARGET_FILE:secret-service-test>)

    add_executable(history-service-test
        history-service-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/temporary_directory.cpp)
    target_link_libraries(history-service-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME history-service-test COMMAND $<TARGET_FILE:history-service-test>)

    add_executable(history-output-test
        history-output-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/temporary_directory.cpp)
    target_link_libraries(history-output-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME history-output-test COMMAND $<TARGET_FILE:history-output-test>)

    # JobU SQLite-backed run, attempt, and output repository test
    set(TARGET "jobu-run-attempt-repository-test")
    add_executable(${TARGET}
        jobu-run-attempt-repository-test.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # Phase 7 persisted-state fixtures use the real schema and existing domain decoders.
    add_executable(recovery-fixture-test
        recovery-fixture-test.cpp
        support/recovery_fixture.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-fixture-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-fixture-test COMMAND $<TARGET_FILE:recovery-fixture-test>)

    add_executable(recovery-repository-test
        recovery-repository-test.cpp
        support/recovery_fixture.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-repository-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-repository-test COMMAND $<TARGET_FILE:recovery-repository-test>)

    add_executable(recovery-interruption-test
        recovery-interruption-test.cpp
        support/recovery_fixture.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-interruption-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-interruption-test COMMAND $<TARGET_FILE:recovery-interruption-test>)

    # Recovery retry persistence and compatibility with normal dispatch.
    add_executable(recovery-retry-test
        recovery-retry-test.cpp
        support/recovery_fixture.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-retry-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-retry-test COMMAND $<TARGET_FILE:recovery-retry-test>)

    add_executable(recovery-repair-test
        recovery-repair-test.cpp
        support/recovery_fixture.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-repair-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-repair-test COMMAND $<TARGET_FILE:recovery-repair-test>)

    add_executable(recovery-service-test
        recovery-service-test.cpp
        support/recovery_fixture.cpp
        support/fake_attempt_executor.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(recovery-service-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-service-test COMMAND $<TARGET_FILE:recovery-service-test>)

    add_executable(recovery-transaction-fault-test
        recovery-transaction-fault-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/temporary_directory.cpp)
    target_link_libraries(recovery-transaction-fault-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME recovery-transaction-fault-test COMMAND $<TARGET_FILE:recovery-transaction-fault-test>)

    add_executable(management-transaction-fault-test
        management-transaction-fault-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/temporary_directory.cpp)
    target_link_libraries(management-transaction-fault-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME management-transaction-fault-test COMMAND $<TARGET_FILE:management-transaction-fault-test>)

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

    # JobU dispatch/completion fault matrix using a forwarding driver over real SQLite.
    add_executable(scheduler-transaction-fault-test
        scheduler-transaction-fault-test.cpp support/fault_database_driver.cpp
        support/recovery_fixture.cpp support/fake_attempt_executor.cpp support/temporary_directory.cpp)
    target_link_libraries(scheduler-transaction-fault-test PRIVATE jobu-sqlite Catch2::Catch2WithMain)
    add_test(NAME scheduler-transaction-fault-test COMMAND $<TARGET_FILE:scheduler-transaction-fault-test>)

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

    # JobU SQLite-backed real HTTP scheduler integration test
    set(TARGET "http-scheduler-integration-test")
    add_executable(${TARGET}
        http-scheduler-integration-test.cpp
        support/http_test_server.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET}
        PRIVATE
            jobu-sqlite
            jobu-http
            net-http
            OpenSSL::SSL
            OpenSSL::Crypto
            Catch2::Catch2WithMain
    )
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

    # Mixed-runner scheduling through the real POSIX Process and HTTP executors.
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
        set(TARGET "cli-scheduler-integration-test")
        add_executable(${TARGET}
            cli-scheduler-integration-test.cpp
            support/http_test_server.cpp
            support/temporary_directory.cpp
        )
        target_link_libraries(${TARGET}
            PRIVATE
                jobu-sqlite
                jobu-cli
                jobu-http
                net-http
                SQLite3::SQLite3
                OpenSSL::SSL
                OpenSSL::Crypto
                Catch2::Catch2WithMain
        )
        target_compile_definitions(${TARGET} PRIVATE PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
        add_dependencies(${TARGET} process-test-helper)
        add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
        set_tests_properties(${TARGET} PROPERTIES TIMEOUT 60)
    endif()

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
        support/fault_database_driver.cpp
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
        attempt-executor-group
        attribute-registry
        cron
        history
        history-json
        history-service
        job
        management
        management-json
        management-rpc
        queue
        recovery
        run
        scheduler
        secret
        secret-service
        secret-provider
        system-info-rpc
        utc-timestamp
)
    set(TARGET "jobu-${JOBU_DOMAIN_HEADER}-public-header-test")
    add_executable(${TARGET} "${TARGET}.cpp")
    target_link_libraries(${TARGET} PRIVATE jobu)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
endforeach()

# JobU HTTP attempt executor public header must compile as the first include.
set(TARGET "jobu-http-public-header-test")
add_executable(${TARGET} jobu-http-public-header-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu-http)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

# JobU CLI attempt executor public header must compile as the first include.
set(TARGET "jobu-cli-public-header-test")
add_executable(${TARGET} jobu-cli-public-header-test.cpp)
target_link_libraries(${TARGET} PRIVATE jobu-cli)
add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)

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

# EventLoop public header must compile as the first include.
set(TARGET "core-event-loop-public-header-test")
add_executable(${TARGET} core-event-loop-public-header-test.cpp)
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
if(TARGET jobud-signal)
    # Compile the same relay with deterministic handler checkpoints only in this isolated helper.
    # The production jobud-signal library contains neither hooks nor runtime test switches.
    add_executable(jobud-signal-test-helper
        jobud-signal-test-helper.cpp
        ../src/jobud/shutdown_signal_posix.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_sources(jobud-signal-test-helper PRIVATE ../src/jobud/shutdown_signal_pipe_macos.cpp)
    else()
        target_sources(jobud-signal-test-helper PRIVATE ../src/jobud/shutdown_signal_pipe_linux.cpp)
    endif()
    target_compile_definitions(jobud-signal-test-helper PRIVATE JOBUD_SIGNAL_TESTING
        JOBUD_SIGNAL_TEST_HELPER="$<TARGET_FILE:jobud-signal-test-helper>")
    target_include_directories(jobud-signal-test-helper PRIVATE ../src/jobud)
    target_link_libraries(jobud-signal-test-helper PRIVATE core Catch2::Catch2)
    add_executable(jobud-signal-test jobud-signal-test.cpp)
    target_link_libraries(jobud-signal-test PRIVATE core Catch2::Catch2WithMain)
    target_compile_definitions(jobud-signal-test PRIVATE
        JOBUD_SIGNAL_TEST_HELPER="$<TARGET_FILE:jobud-signal-test-helper>")
    add_dependencies(jobud-signal-test jobud-signal-test-helper)
    add_test(NAME jobud-signal-test COMMAND $<TARGET_FILE:jobud-signal-test>)
    set_tests_properties(jobud-signal-test PROPERTIES TIMEOUT 60)
endif()

if(TARGET jobud)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
        add_executable(jobud-runtime-test
            jobud-runtime-test.cpp
            support/fault_database_driver.cpp
            support/fake_http_client.cpp
            support/memory_io_device.cpp
            support/recovery_fixture.cpp
            support/temporary_directory.cpp
        )
        target_link_libraries(jobud-runtime-test PRIVATE jobud-runtime jobu-sqlite jobu-http Catch2::Catch2WithMain)
        add_test(NAME jobud-runtime-test COMMAND $<TARGET_FILE:jobud-runtime-test>)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            # The default macOS temporary directory can exceed the Unix socket path limit.
            set_tests_properties(jobud-runtime-test PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
        endif()
        add_executable(jobud-startup-test
            jobud-startup-test.cpp
            ../src/jobud/startup_priv.cpp
            ../src/jobud/composition_priv.cpp
            support/fake_http_client.cpp
            support/temporary_directory.cpp
        )
        target_include_directories(jobud-startup-test PRIVATE ../src/jobud)
        target_link_libraries(jobud-startup-test PRIVATE jobu-sqlite jobu-http jobu-cli Catch2::Catch2WithMain)
        add_test(NAME jobud-startup-test COMMAND $<TARGET_FILE:jobud-startup-test>)
    endif()
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

add_executable(jobuctl-command-line-test jobuctl-command-line-test.cpp)
target_link_libraries(jobuctl-command-line-test PRIVATE jobuctl-private Catch2::Catch2WithMain)
add_test(NAME jobuctl-command-line-test COMMAND $<TARGET_FILE:jobuctl-command-line-test>)

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    add_executable(jobuctl-help-test jobuctl-help-test.cpp support/temporary_directory.cpp)
    target_link_libraries(jobuctl-help-test PRIVATE core Catch2::Catch2WithMain)
    target_compile_definitions(jobuctl-help-test PRIVATE JOBUCTL_EXECUTABLE="$<TARGET_FILE:jobuctl>")
    add_dependencies(jobuctl-help-test jobuctl)
    add_test(NAME jobuctl-help-test COMMAND $<TARGET_FILE:jobuctl-help-test>)
    set_tests_properties(jobuctl-help-test PROPERTIES TIMEOUT 60)

    add_executable(jobuctl-session-test jobuctl-session-test.cpp support/temporary_directory.cpp)
    target_link_libraries(jobuctl-session-test PRIVATE jobu Catch2::Catch2WithMain)
    target_compile_definitions(jobuctl-session-test PRIVATE JOBUCTL_EXECUTABLE="$<TARGET_FILE:jobuctl>")
    add_dependencies(jobuctl-session-test jobuctl)
    add_test(NAME jobuctl-session-test COMMAND $<TARGET_FILE:jobuctl-session-test>)
    set_tests_properties(jobuctl-session-test PROPERTIES TIMEOUT 60)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(jobuctl-session-test PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()

    # The real client talks to a controlled local peer through the native Process backend.
    add_executable(jobuctl-cli-creation-test jobuctl-cli-creation-test.cpp support/temporary_directory.cpp)
    target_link_libraries(jobuctl-cli-creation-test PRIVATE jobu Catch2::Catch2WithMain)
    target_compile_definitions(jobuctl-cli-creation-test PRIVATE JOBUCTL_EXECUTABLE="$<TARGET_FILE:jobuctl>")
    add_dependencies(jobuctl-cli-creation-test jobuctl)
    add_test(NAME jobuctl-cli-creation-test COMMAND $<TARGET_FILE:jobuctl-cli-creation-test>)
    set_tests_properties(jobuctl-cli-creation-test PROPERTIES TIMEOUT 60)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        # The default macOS temporary directory can exceed the Unix socket path limit.
        set_tests_properties(jobuctl-cli-creation-test PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

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

# Private child-exit watch tests, including real pidfds on Linux
add_executable(event-loop-process-watch-test event-loop-process-watch-test.cpp)
target_link_libraries(event-loop-process-watch-test PRIVATE core Catch2::Catch2WithMain)
add_test(NAME event-loop-process-watch-test COMMAND $<TARGET_FILE:event-loop-process-watch-test>)
set_tests_properties(event-loop-process-watch-test PROPERTIES TIMEOUT 30)

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
target_link_libraries(${TARGET} PRIVATE net-http OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
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

    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
        # Platform daemon HTTP scheduling, management wakeup, and durable output integration test
        find_package(SQLite3 REQUIRED)
        if(NOT TARGET SQLite3::SQLite3)
            add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
        endif()
        set(TARGET "jobud-http-integration-test")
        add_executable(${TARGET}
            jobud-http-integration-test.cpp
            support/http_test_server.cpp
            support/temporary_directory.cpp
        )
        target_link_libraries(${TARGET}
            PRIVATE
                core
                SQLite3::SQLite3
                OpenSSL::SSL
                OpenSSL::Crypto
        )
        add_dependencies(${TARGET} jobud jobuctl)
        add_test(NAME ${TARGET}
            COMMAND $<TARGET_FILE:${TARGET}> $<TARGET_FILE:jobud> $<TARGET_FILE:jobuctl>
        )
        set_tests_properties(${TARGET} PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
            set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
        endif()
    endif()
endif()

# Native daemon crash/restart evidence with committed SQLite state assertions.
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND TARGET jobud)
    add_executable(jobud-recovery-integration-test
        jobud-recovery-integration-test.cpp
        support/recovery_fixture.cpp
        support/http_test_server.cpp
        support/temporary_directory.cpp)
    target_link_libraries(jobud-recovery-integration-test PRIVATE
        jobu-sqlite net rpc OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    target_compile_definitions(jobud-recovery-integration-test PRIVATE
        JOBUD_EXECUTABLE="$<TARGET_FILE:jobud>"
        JOBUCTL_EXECUTABLE="$<TARGET_FILE:jobuctl>"
        PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(jobud-recovery-integration-test jobud jobuctl process-test-helper)
    add_test(NAME jobud-recovery-integration-test COMMAND $<TARGET_FILE:jobud-recovery-integration-test>)
    set_tests_properties(jobud-recovery-integration-test PROPERTIES TIMEOUT 120)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set_tests_properties(jobud-recovery-integration-test PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

# Native immediate shutdown evidence with mixed HTTP and CLI work.
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND TARGET jobud)
    add_executable(jobud-shutdown-test-helper
        jobud-shutdown-test-helper.cpp
        support/shutdown_fixture.cpp
        support/recovery_fixture.cpp
        support/http_test_server.cpp
        support/fault_database_driver.cpp
        support/memory_io_device.cpp
        support/temporary_directory.cpp)
    target_link_libraries(jobud-shutdown-test-helper PRIVATE
        jobud-runtime jobu-sqlite jobu-http jobu-cli net-http OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    target_compile_definitions(jobud-shutdown-test-helper PRIVATE
        PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(jobud-shutdown-test-helper process-test-helper)

    add_executable(jobud-shutdown-integration-test
        jobud-shutdown-integration-test.cpp
        support/shutdown_fixture.cpp
        support/recovery_fixture.cpp
        support/http_test_server.cpp
        support/temporary_directory.cpp)
    target_link_libraries(jobud-shutdown-integration-test PRIVATE
        jobu-sqlite net rpc OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    target_compile_definitions(jobud-shutdown-integration-test PRIVATE
        JOBUD_EXECUTABLE="$<TARGET_FILE:jobud>"
        JOBUD_SHUTDOWN_TEST_HELPER="$<TARGET_FILE:jobud-shutdown-test-helper>"
        PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(jobud-shutdown-integration-test jobud jobud-shutdown-test-helper process-test-helper)
    add_test(NAME jobud-shutdown-integration-test COMMAND $<TARGET_FILE:jobud-shutdown-integration-test>)
    set_tests_properties(jobud-shutdown-integration-test PROPERTIES TIMEOUT 120)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set_tests_properties(jobud-shutdown-integration-test PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()

# Real POSIX daemon CLI execution; the root override requires an explicit isolated-test opt-in.
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin" AND TARGET jobud)
    set(TARGET "jobud-cli-integration-test")
    add_executable(${TARGET}
        jobud-cli-integration-test.cpp
        support/http_test_server.cpp
        support/temporary_directory.cpp
    )
    target_link_libraries(${TARGET} PRIVATE net rpc SQLite3::SQLite3
        OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    target_compile_definitions(${TARGET} PRIVATE
        JOBUD_EXECUTABLE="$<TARGET_FILE:jobud>"
        JOBUCTL_EXECUTABLE="$<TARGET_FILE:jobuctl>"
        PROCESS_TEST_HELPER="$<TARGET_FILE:process-test-helper>")
    add_dependencies(${TARGET} jobud jobuctl process-test-helper)
    add_test(NAME ${TARGET} COMMAND $<TARGET_FILE:${TARGET}>)
    set_tests_properties(${TARGET} PROPERTIES TIMEOUT 90)
    if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        set_tests_properties(${TARGET} PROPERTIES ENVIRONMENT "TMPDIR=/tmp")
    endif()
endif()
