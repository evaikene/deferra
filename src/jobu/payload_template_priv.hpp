#pragma once

#include "job_validation_priv.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jb::jobu::detail {

inline constexpr std::size_t maximum_payload_secret_references{256};

struct SecretReference {
    std::string secret_name;
    std::string field_path;

    auto operator==(SecretReference const&) const -> bool = default;
};

/// Uses the same case-preserving canonical identifier contract as secret storage.
[[nodiscard]] auto is_valid_secret_name(std::string_view name) noexcept -> bool;

/// Escapes one JSON pointer component; callers supply separators and array indices.
[[nodiscard]] auto escape_payload_pointer_component(std::string_view component) -> std::string;

/// Collects recognized occurrences during one validation pass; performs no database access.
/// Results are usable only after the entire template has passed validation.
class PayloadTemplateReferences final {
public:
    [[nodiscard]] auto add(jb::core::JsonValue const& reference, std::string field_path) -> JobPayloadIssue;
    [[nodiscard]] auto take() && -> std::vector<SecretReference>;

private:
    std::size_t                  _occurrences{};
    std::vector<SecretReference> _references;
};

/// A borrowed literal, or an unresolved value whose bytes are deliberately unknown.
/// The source JSON must outlive this view. Absence is not an empty-string substitution.
using PayloadText = std::optional<std::string_view>;

/// With no collector, accepts only strings and preserves the concrete decoder's error reason.
[[nodiscard]] auto parse_payload_text(jb::core::JsonValue const& value,
                                      JobPayloadIssue            literal_issue,
                                      PayloadTemplateReferences* references,
                                      std::string field_path) -> jb::core::Result<PayloadText, JobPayloadIssue>;

/// Validates all recognized fields and extracts references without modifying the original JSON.
/// The document byte bound is applied separately by validate_and_serialize_job_payload or durable JSON decoding.
[[nodiscard]] auto validate_payload_template(JobType type, jb::core::JsonValue const& payload)
    -> jb::core::Result<std::vector<SecretReference>, JobPayloadIssue>;

} // namespace jb::jobu::detail
