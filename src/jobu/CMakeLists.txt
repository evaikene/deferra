set(TARGET jobu)

message("** Setting up ${TARGET}")

add_library(${TARGET} STATIC)

target_include_directories(${TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(${TARGET}
    PRIVATE
        attempt_executor_group.cpp
        attribute.cpp
        attribute_codec_priv.cpp
        attribute_codec_priv.hpp
        attribute_registry.cpp
        attempt_repository_priv.cpp
        attempt_repository_priv.hpp
        cli_capture_priv.cpp
        cli_capture_priv.hpp
        cli_exit_policy_priv.cpp
        cli_exit_policy_priv.hpp
        cli_job_payload_priv.cpp
        cli_job_payload_priv.hpp
        cron.cpp
        cron_expression_priv.cpp
        cron_expression_priv.hpp
        cron_timezone_priv.cpp
        cron_timezone_priv.hpp
        domain_storage_priv.cpp
        domain_storage_priv.hpp
        http_job_payload_priv.cpp
        http_job_payload_priv.hpp
        http_retry_priv.cpp
        http_retry_priv.hpp
        idempotency_codec_priv.cpp
        idempotency_codec_priv.hpp
        idempotency_repository_priv.cpp
        idempotency_repository_priv.hpp
        job_repository_priv.cpp
        job_repository_priv.hpp
        job_validation_priv.cpp
        job_validation_priv.hpp
        jobu.cpp
        management.cpp
        management_json.cpp
        management_rpc.cpp
        payload_template_priv.cpp
        payload_template_priv.hpp
        queue_repository_priv.cpp
        queue_repository_priv.hpp
        queue_validation_priv.cpp
        queue_validation_priv.hpp
        retry_policy_priv.cpp
        retry_policy_priv.hpp
        recurrence_priv.cpp
        recurrence_priv.hpp
        recovery.cpp
        recovery_priv.hpp
        recovery_repository_priv.cpp
        recovery_repository_priv.hpp
        recovery_retry_priv.cpp
        recovery_retry_priv.hpp
        retention_repository_priv.cpp
        retention_repository_priv.hpp
        run_repository_priv.cpp
        run_repository_priv.hpp
        scheduler.cpp
        scheduler_core_priv.cpp
        scheduler_core_priv.hpp
        scheduler_dispatch_priv.cpp
        scheduler_dispatch_priv.hpp
        scheduler_repository_priv.cpp
        scheduler_repository_priv.hpp
        secret_repository_priv.cpp
        secret_repository_priv.hpp
        storage_failure_priv.cpp
        storage_failure_priv.hpp
        system_info.cpp
        system_info_rpc.cpp
        text_validation_priv.cpp
        text_validation_priv.hpp
        utc_timestamp.cpp
    PUBLIC
        attempt.hpp
        attempt_executor.hpp
        attempt_executor_group.hpp
        attribute.hpp
        attribute_registry.hpp
        cron.hpp
        job.hpp
        jobu.hpp
        management.hpp
        management_json.hpp
        management_rpc.hpp
        queue.hpp
        recovery.hpp
        run.hpp
        scheduler.hpp
        secret.hpp
        system_info.hpp
        system_info_rpc.hpp
        utc_timestamp.hpp
)

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET}
    PUBLIC
        core
        net
        db
        rpc
)

add_subdirectory(http)
add_subdirectory(cli)

if(TARGET db-sqlite)
    add_subdirectory(sqlite)
endif()
