#include "job_payload_admission_priv.hpp"

#include "http_job_method_priv.hpp"
#include "job_validation_priv.hpp"

#include <fmt/format.h>

#include <string>
#include <utility>

namespace jb::jobu::detail {

namespace {

auto payload_error(JobPayloadIssue issue) -> jb::core::Error
{
    // Preserve management's established size-error category and protocol identity.
    if (issue == JobPayloadIssue::TooLarge) {
        return {
            .category = jb::core::ErrorCategory::ResourceExhausted,
            .code     = "jobu.protocol.value_too_large",
            .message  = "Job payload exceeds its size limit",
            .detail   = "reason=too_large",
        };
    }
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.job.invalid_payload",
        .message  = "Job runner type or payload is structurally invalid",
        .detail   = fmt::format("reason={}", job_payload_issue_text(issue)),
    };
}

} // anonymous namespace

auto normalize_submitted_job_payload(JobType type, jb::core::JsonValue payload)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>
{
    using AdmissionResult = jb::core::Result<jb::core::JsonValue, jb::core::Error>;

    // Admission owns this copy. Never apply this policy while decoding stored definitions or run snapshots.
    if (type == JobType::Http && payload.is_object()) {
        auto&      object = std::get<jb::core::JsonValue::Object>(payload.data);
        auto const found  = object.find("method");
        auto       method = HttpJobMethod::Get;
        if (found != object.end()) {
            if (!found->second.is_string()) {
                return AdmissionResult::failure({
                    .category = jb::core::ErrorCategory::InvalidArgument,
                    .code     = "jobu.job.invalid_payload",
                    .message =
                        fmt::format("HTTP method must be a string. Supported methods: {}.", supported_http_job_methods),
                    .detail = "reason=invalid_method",
                });
            }
            auto parsed = parse_submitted_http_method(found->second.as_string());
            if (!parsed) {
                return AdmissionResult::failure(std::move(parsed).error());
            }
            method = *parsed;
        }
        object.insert_or_assign("method", jb::core::JsonValue{.data = std::string{http_method_text(method)}});
    }

    // Canonicalization precedes all shared checks, including the HEAD body rule and durable document limit.
    auto validated = validate_and_serialize_job_payload(type, payload);
    if (!validated) {
        return AdmissionResult::failure(payload_error(validated.error()));
    }
    return AdmissionResult::success(std::move(payload));
}

} // namespace jb::jobu::detail
