#include "job_validation_priv.hpp"

#include "cli_job_payload_priv.hpp"
#include "http_job_payload_priv.hpp"
#include "json.hpp"
#include "text_validation_priv.hpp"

#include <cstddef>
#include <utility>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t kMaximumJobNameBytes = 256;

} // anonymous namespace

auto is_valid_job_name(std::string_view name) noexcept -> bool
{
    return name.size() <= kMaximumJobNameBytes && is_valid_utf8(name) && !has_ascii_control(name);
}

ValidatedJobPayload::ValidatedJobPayload(std::string serialized) noexcept
    : _serialized{std::move(serialized)}
{}

auto job_payload_structure_issue(JobType type, jb::core::JsonValue const& payload) -> JobPayloadIssue
{
    if (!payload.is_object()) {
        return JobPayloadIssue::NotObject;
    }

    switch (type) {
        case JobType::Cli: {
            auto decoded = decode_cli_job_payload(payload);
            return decoded ? JobPayloadIssue::None : decoded.error();
        }
        case JobType::Http: {
            auto decoded = decode_http_job_payload(payload);
            return decoded ? JobPayloadIssue::None : decoded.error();
        }
    }
    return JobPayloadIssue::UnknownType;
}

auto validate_and_serialize_job_payload(JobType type, jb::core::JsonValue const& payload)
    -> jb::core::Result<ValidatedJobPayload, JobPayloadIssue>
{
    using ValidationResult = jb::core::Result<ValidatedJobPayload, JobPayloadIssue>;

    // Bound the original document before decoding owning body/header values.
    auto serialized = jb::core::serialize_json(payload);
    if (!serialized) {
        return ValidationResult::failure(JobPayloadIssue::InvalidJson);
    }
    if (serialized->size() > maximum_job_document_bytes) {
        return ValidationResult::failure(JobPayloadIssue::TooLarge);
    }
    auto const issue = job_payload_structure_issue(type, payload);
    if (issue != JobPayloadIssue::None) {
        return ValidationResult::failure(issue);
    }
    return ValidationResult::success(ValidatedJobPayload{std::move(serialized).value()});
}

auto job_payload_issue_text(JobPayloadIssue issue) noexcept -> std::string_view
{
    switch (issue) {
        case JobPayloadIssue::None:
            return "none";
        case JobPayloadIssue::UnknownType:
            return "unknown_type";
        case JobPayloadIssue::NotObject:
            return "not_object";
        case JobPayloadIssue::MissingCommand:
            return "missing_command";
        case JobPayloadIssue::InvalidCommand:
            return "invalid_command";
        case JobPayloadIssue::InvalidArguments:
            return "invalid_arguments";
        case JobPayloadIssue::InvalidWorkingDirectory:
            return "invalid_working_directory";
        case JobPayloadIssue::InvalidEnvironment:
            return "invalid_environment";
        case JobPayloadIssue::InvalidPath:
            return "invalid_path";
        case JobPayloadIssue::PreparedRequestTooLarge:
            return "prepared_request_too_large";
        case JobPayloadIssue::InvalidExpectedExitCodes:
            return "invalid_expected_exit_codes";
        case JobPayloadIssue::MissingUrl:
            return "missing_url";
        case JobPayloadIssue::InvalidUrl:
            return "invalid_url";
        case JobPayloadIssue::InvalidMethod:
            return "invalid_method";
        case JobPayloadIssue::InvalidHeaders:
            return "invalid_headers";
        case JobPayloadIssue::InvalidBody:
            return "invalid_body";
        case JobPayloadIssue::InvalidExpectedStatuses:
            return "invalid_expected_statuses";
        case JobPayloadIssue::InvalidHttpRequest:
            return "invalid_http_request";
        case JobPayloadIssue::InvalidJson:
            return "invalid_json";
        case JobPayloadIssue::TooLarge:
            return "too_large";
    }
    return "unknown_issue";
}

} // namespace jb::jobu::detail
