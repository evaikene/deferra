#include "job_validation_priv.hpp"

#include "json.hpp"
#include "text_validation_priv.hpp"

#include <cstddef>
#include <utility>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t kMaximumJobNameBytes = 256;

auto member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> jb::rpc::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto structurally_valid_cli(jb::rpc::JsonValue::Object const& object) -> JobPayloadIssue
{
    auto const* command = member(object, "command");
    if (command == nullptr || !command->is_string() || command->as_string().empty()) {
        return JobPayloadIssue::MissingCommand;
    }
    auto const* arguments = member(object, "arguments");
    if (arguments == nullptr) {
        return JobPayloadIssue::None;
    }
    if (!arguments->is_array()) {
        return JobPayloadIssue::InvalidArguments;
    }
    for (auto const& argument : arguments->as_array()) {
        if (!argument.is_string()) {
            return JobPayloadIssue::InvalidArguments;
        }
    }
    return JobPayloadIssue::None;
}

auto structurally_valid_http(jb::rpc::JsonValue::Object const& object) -> JobPayloadIssue
{
    auto const* url = member(object, "url");
    if (url == nullptr || !url->is_string() || url->as_string().empty()) {
        return JobPayloadIssue::MissingUrl;
    }
    auto const* method = member(object, "method");
    if (method != nullptr && (!method->is_string() || method->as_string().empty())) {
        return JobPayloadIssue::InvalidMethod;
    }
    return JobPayloadIssue::None;
}

} // anonymous namespace

auto is_valid_job_name(std::string_view name) noexcept -> bool
{
    return name.size() <= kMaximumJobNameBytes && is_valid_utf8(name) && !has_ascii_control(name);
}

ValidatedJobPayload::ValidatedJobPayload(std::string serialized) noexcept
    : _serialized{std::move(serialized)}
{}

auto job_payload_structure_issue(JobType type, jb::rpc::JsonValue const& payload) -> JobPayloadIssue
{
    if (!payload.is_object()) {
        return JobPayloadIssue::NotObject;
    }

    switch (type) {
        case JobType::Cli:
            return structurally_valid_cli(payload.as_object());
        case JobType::Http:
            return structurally_valid_http(payload.as_object());
    }
    return JobPayloadIssue::UnknownType;
}

auto validate_and_serialize_job_payload(JobType type, jb::rpc::JsonValue const& payload)
    -> jb::core::Result<ValidatedJobPayload, JobPayloadIssue>
{
    using ValidationResult = jb::core::Result<ValidatedJobPayload, JobPayloadIssue>;

    auto const issue = job_payload_structure_issue(type, payload);
    if (issue != JobPayloadIssue::None) {
        return ValidationResult::failure(issue);
    }

    auto serialized = jb::rpc::serialize_json(payload);
    if (!serialized) {
        return ValidationResult::failure(JobPayloadIssue::InvalidJson);
    }
    if (serialized->size() > maximum_job_document_bytes) {
        return ValidationResult::failure(JobPayloadIssue::TooLarge);
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
        case JobPayloadIssue::InvalidArguments:
            return "invalid_arguments";
        case JobPayloadIssue::MissingUrl:
            return "missing_url";
        case JobPayloadIssue::InvalidMethod:
            return "invalid_method";
        case JobPayloadIssue::InvalidJson:
            return "invalid_json";
        case JobPayloadIssue::TooLarge:
            return "too_large";
    }
    return "unknown_issue";
}

} // namespace jb::jobu::detail
