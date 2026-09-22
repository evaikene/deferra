#include "payload_template_priv.hpp"

#include "attribute.hpp"
#include "cli_job_payload_priv.hpp"
#include "http_job_payload_priv.hpp"

#include <algorithm>
#include <utility>

namespace jb::jobu::detail {

auto is_valid_secret_name(std::string_view name) noexcept -> bool
{
    return name.size() <= 128U && is_valid_attribute_name(name);
}

auto escape_payload_pointer_component(std::string_view component) -> std::string
{
    std::string escaped;
    for (char character : component) {
        if (character == '~') {
            escaped += "~0";
        }
        else if (character == '/') {
            escaped += "~1";
        }
        else {
            escaped += character;
        }
    }
    return escaped;
}

auto PayloadTemplateReferences::add(jb::core::JsonValue const& reference, std::string field_path) -> JobPayloadIssue
{
    if (!reference.is_object()) {
        return JobPayloadIssue::InvalidSecretReference;
    }
    auto const& object = reference.as_object();
    auto const  name   = object.find("secret");
    if (object.size() != 1U || name == object.end() || !name->second.is_string()) {
        return JobPayloadIssue::InvalidSecretReference;
    }
    if (!is_valid_secret_name(name->second.as_string())) {
        return JobPayloadIssue::InvalidSecretName;
    }
    // Count occurrences before deduplication: repeated names cannot evade the work bound.
    if (_occurrences == maximum_payload_secret_references) {
        return JobPayloadIssue::TooManySecretReferences;
    }
    ++_occurrences;
    auto entry = SecretReference{.secret_name = name->second.as_string(), .field_path = std::move(field_path)};
    if (std::ranges::find(_references, entry) == _references.end()) {
        _references.push_back(std::move(entry));
    }
    return JobPayloadIssue::None;
}

auto PayloadTemplateReferences::take() && -> std::vector<SecretReference>
{
    return std::move(_references);
}

auto parse_payload_text(jb::core::JsonValue const& value,
                        JobPayloadIssue            literal_issue,
                        PayloadTemplateReferences* references,
                        std::string                field_path) -> jb::core::Result<PayloadText, JobPayloadIssue>
{
    using Parsed = jb::core::Result<PayloadText, JobPayloadIssue>;
    if (value.is_string()) {
        return Parsed::success(value.as_string());
    }
    if (references == nullptr || !value.is_object()) {
        return Parsed::failure(literal_issue);
    }
    auto const issue = references->add(value, std::move(field_path));
    if (issue != JobPayloadIssue::None) {
        return Parsed::failure(issue);
    }
    return Parsed::success(std::nullopt);
}

auto validate_payload_template(JobType type, jb::core::JsonValue const& payload)
    -> jb::core::Result<std::vector<SecretReference>, JobPayloadIssue>
{
    using Validated = jb::core::Result<std::vector<SecretReference>, JobPayloadIssue>;
    if (!payload.is_object()) {
        return Validated::failure(JobPayloadIssue::NotObject);
    }
    PayloadTemplateReferences references;
    auto                      issue = JobPayloadIssue::UnknownType;
    switch (type) {
        case JobType::Cli:
            issue = validate_cli_payload_template(payload, references);
            break;
        case JobType::Http:
            issue = validate_http_payload_template(payload, references);
            break;
    }
    if (issue != JobPayloadIssue::None) {
        return Validated::failure(issue);
    }
    return Validated::success(std::move(references).take());
}

} // namespace jb::jobu::detail
