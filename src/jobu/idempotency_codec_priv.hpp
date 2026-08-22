#pragma once

#include "management.hpp"

#include <string>
#include <string_view>

namespace jb::jobu::detail {

[[nodiscard]] auto encode_queue_create_idempotency_request(CreateQueueRequest const& request,
                                                           AttributeRegistry const&  attributes)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto validate_queue_create_idempotency_request(std::string_view         request_json,
                                                             AttributeRegistry const& attributes)
    -> jb::core::Result<void, jb::core::Error>;
[[nodiscard]] auto encode_queue_idempotency_result(Queue const& queue, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto decode_queue_idempotency_result(std::string_view result_json, AttributeRegistry const& attributes)
    -> jb::core::Result<Queue, jb::core::Error>;

[[nodiscard]] auto encode_job_create_idempotency_request(CreateJobRequest const&  request,
                                                         jb::core::Uuid const&    resolved_queue_id,
                                                         AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto validate_job_create_idempotency_request(std::string_view         request_json,
                                                           AttributeRegistry const& attributes)
    -> jb::core::Result<void, jb::core::Error>;
[[nodiscard]] auto encode_job_idempotency_result(JobDefinition const& job, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto decode_job_idempotency_result(std::string_view result_json, AttributeRegistry const& attributes)
    -> jb::core::Result<JobDefinition, jb::core::Error>;

[[nodiscard]] auto encode_run_now_idempotency_request(RunNowRequest const& request)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto validate_run_now_idempotency_request(std::string_view request_json)
    -> jb::core::Result<void, jb::core::Error>;
[[nodiscard]] auto encode_run_now_idempotency_result(JobRun const& run, AttributeRegistry const& attributes)
    -> jb::core::Result<std::string, jb::core::Error>;
[[nodiscard]] auto decode_run_now_idempotency_result(std::string_view result_json, AttributeRegistry const& attributes)
    -> jb::core::Result<JobRun, jb::core::Error>;

} // namespace jb::jobu::detail
