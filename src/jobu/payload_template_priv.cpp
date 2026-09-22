#include "payload_template_priv.hpp"

#include "attribute.hpp"
#include "cli_job_payload_priv.hpp"
#include "http_job_payload_priv.hpp"
#include "secret_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "text_validation_priv.hpp"

#include <algorithm>
#include <map>
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

namespace {

using PreparationResult = jb::core::Result<jb::core::JsonValue, PayloadPreparationFailure>;
using FailureKind       = PayloadPreparationFailureKind;
using SecretValues      = std::map<std::string, jb::core::ByteBuffer, std::less<>>;

// These fixed diagnostics are the only ordinary errors permitted to become attempt results.
auto preparation_failure(FailureKind             kind,
                         jb::core::ErrorCategory category,
                         std::string_view        code,
                         std::string_view        message) -> PayloadPreparationFailure
{
    return {
        .kind  = kind,
        .error = {.category = category, .code = std::string{code}, .message = std::string{message}}
    };
}

auto invalid_secret_value() -> PayloadPreparationFailure
{
    return preparation_failure(FailureKind::Ordinary,
                               jb::core::ErrorCategory::InvalidArgument,
                               "jobu.secret.invalid_value",
                               "Secret value is invalid for its destination");
}

auto resolved_payload_too_large() -> PayloadPreparationFailure
{
    return preparation_failure(FailureKind::Ordinary,
                               jb::core::ErrorCategory::ResourceExhausted,
                               "jobu.secret.resolved_payload_too_large",
                               "Resolved payload exceeds execution limits");
}

auto invalid_persisted_template() -> PayloadPreparationFailure
{
    return preparation_failure(FailureKind::PersistedData,
                               jb::core::ErrorCategory::Internal,
                               "jobu.storage.invalid_json",
                               "Persisted payload template is invalid");
}

auto unexpected_provider_failure() -> PayloadPreparationFailure
{
    return preparation_failure(FailureKind::Provider,
                               jb::core::ErrorCategory::Internal,
                               "jobu.secret.provider_failed",
                               "Secret provider cannot safely prepare execution");
}

auto provider_failure(jb::core::Error const& error) -> PayloadPreparationFailure
{
    // Provider codes are trusted identifiers by contract; text and detail never cross this boundary.
    if (error.code == "jobu.secret.not_found") {
        return preparation_failure(FailureKind::Ordinary,
                                   jb::core::ErrorCategory::NotFound,
                                   "jobu.secret.not_found",
                                   "Secret was not found");
    }
    if (error.code.starts_with("db.")) {
        return {.kind = FailureKind::Storage, .error = sanitized_storage_error(error, StorageOperation::Dispatch)};
    }
    if (error.code.starts_with("jobu.storage.") && error.code != "jobu.storage.invalid_limit") {
        return {.kind = FailureKind::PersistedData,
                .error =
                    sanitized_storage_error(error, StorageOperation::Dispatch, StorageFailureOrigin::PersistedData)};
    }
    return unexpected_provider_failure();
}

auto encode_body(jb::core::ByteView bytes) -> jb::core::JsonValue
{
    // Always use padded base64: binary secrets must never pass through text decoding.
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        auto const first  = std::to_integer<unsigned>(bytes[offset]);
        auto const second = offset + 1U < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 1U]) : 0U;
        auto const third  = offset + 2U < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 2U]) : 0U;
        encoded.push_back(alphabet[first >> 2U]);
        encoded.push_back(alphabet[((first & 3U) << 4U) | (second >> 4U)]);
        encoded.push_back(offset + 1U < bytes.size() ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=');
        encoded.push_back(offset + 2U < bytes.size() ? alphabet[third & 63U] : '=');
    }
    using Json = jb::core::JsonValue;
    return Json{
        .data = Json::Object{{"encoding", Json{.data = std::string{"base64"}}},
                             {"data", Json{.data = std::move(encoded)}}}
    };
}

auto reference_bytes(jb::core::JsonValue const& value, SecretValues const& values) -> jb::core::ByteView
{
    return values.at(value.as_object().at("secret").as_string());
}

auto substitute_text(jb::core::JsonValue& value, SecretValues const& values) -> bool
{
    if (!value.is_object()) {
        return true;
    }
    auto const text = jb::core::as_string_view(reference_bytes(value, values));
    if (!is_valid_utf8(text) || text.find('\0') != std::string_view::npos) {
        return false;
    }
    value.data = std::string{text};
    return true;
}

auto substitute_references(JobType type, jb::core::JsonValue& payload, SecretValues const& values) -> bool
{
    // The complete template was validated first. Walk only recognized positions, never arbitrary additive objects.
    auto& object = std::get<jb::core::JsonValue::Object>(payload.data);
    if (type == JobType::Cli) {
        if (auto arguments = object.find("arguments"); arguments != object.end()) {
            for (auto& argument : std::get<jb::core::JsonValue::Array>(arguments->second.data)) {
                if (!substitute_text(argument, values)) {
                    return false;
                }
            }
        }
        if (auto environment = object.find("environment"); environment != object.end()) {
            for (auto& [name, value] : std::get<jb::core::JsonValue::Object>(environment->second.data)) {
                if (!substitute_text(value, values)) {
                    return false;
                }
            }
        }
        return true;
    }

    if (auto headers = object.find("headers"); headers != object.end()) {
        for (auto& header : std::get<jb::core::JsonValue::Array>(headers->second.data)) {
            auto& fields = std::get<jb::core::JsonValue::Object>(header.data);
            auto& value  = fields.at("value");
            if (value.is_object()) {
                if (!substitute_text(value, values)) {
                    return false;
                }
                // Explicit false cannot override secrecy inherited from a reference.
                fields.insert_or_assign("sensitive", jb::core::JsonValue{.data = true});
            }
        }
    }
    if (auto body = object.find("body"); body != object.end() && body->second.as_object().contains("secret")) {
        body->second = encode_body(reference_bytes(body->second, values));
    }
    return true;
}

} // namespace

auto prepare_payload_template(JobType type, jb::core::JsonValue const& payload, SecretProvider& provider)
    -> PreparationResult
{
    // Reject damaged durable templates before any provider access, including invalid additive JSON values.
    auto original = jb::core::serialize_json(payload);
    if (!original || original->size() > maximum_job_document_bytes) {
        return PreparationResult::failure(invalid_persisted_template());
    }
    auto references = validate_payload_template(type, payload);
    if (!references) {
        return PreparationResult::failure(invalid_persisted_template());
    }

    // Cache only within this invocation: repeated destinations agree, while later attempts observe rotation.
    SecretValues values;
    for (auto const& reference : *references) {
        if (values.contains(reference.secret_name)) {
            continue;
        }
        auto resolved = provider.resolve(reference.secret_name);
        if (!resolved) {
            return PreparationResult::failure(provider_failure(resolved.error()));
        }
        if (resolved->size() > kMaximumSecretValueBytes) {
            return PreparationResult::failure(unexpected_provider_failure());
        }
        values.emplace(reference.secret_name, std::move(resolved).value());
    }

    auto concrete = payload;
    if (!substitute_references(type, concrete, values)) {
        return PreparationResult::failure(invalid_secret_value());
    }
    auto serialized = jb::core::serialize_json(concrete);
    if (!serialized) {
        return PreparationResult::failure(invalid_secret_value());
    }
    if (serialized->size() > maximum_job_document_bytes) {
        return PreparationResult::failure(resolved_payload_too_large());
    }

    // Reuse execution validation, including the size reserved for injected JobU metadata.
    auto issue = JobPayloadIssue::None;
    if (type == JobType::Cli) {
        auto decoded = decode_cli_job_payload(concrete);
        issue        = decoded ? JobPayloadIssue::None : decoded.error();
    }
    else {
        issue = prepared_http_payload_issue(concrete);
    }
    if (issue == JobPayloadIssue::PreparedRequestTooLarge) {
        return PreparationResult::failure(resolved_payload_too_large());
    }
    if (issue != JobPayloadIssue::None) {
        return PreparationResult::failure(invalid_secret_value());
    }
    return PreparationResult::success(std::move(concrete));
}

} // namespace jb::jobu::detail
