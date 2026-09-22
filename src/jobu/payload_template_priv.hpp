#pragma once

#include "job_validation_priv.hpp"
#include "json.hpp"
#include "result.hpp"
#include "secret_provider.hpp"

#include <cstddef>
#include <cstdint>
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

/// Ordinary preparation failures become terminal attempts only after durable attempt-start commits.
/// Other kinds abort dispatch; the caller owns transaction cleanup, poison checks, and fatal notification.
enum class PayloadPreparationFailureKind : std::uint8_t {
    Ordinary,
    Storage,
    PersistedData,
    Provider
};

struct PayloadPreparationFailure {
    PayloadPreparationFailureKind kind;
    jb::core::Error error; ///< Safe diagnostics only; never contains a resolved value or raw provider diagnostic.
};

/// Produces an owning transient execution payload; never persist the returned JSON.
/// Revalidates the bounded durable template before lookup, then resolves each distinct name once per call.
/// Neither the original template nor storage is modified. No transaction or callbacks are owned by this helper.
/// Ordinary failures use jobu.secret.not_found (NotFound), jobu.secret.invalid_value (InvalidArgument), or
/// jobu.secret.resolved_payload_too_large (ResourceExhausted). Malformed templates are PersistedData failures;
/// Unknown provider failures or values exceeding the provider byte contract use jobu.secret.provider_failed
/// (Internal) and must abort dispatch. Storage/PersistedData errors retain trusted codes with sanitized diagnostics.
[[nodiscard]] auto prepare_payload_template(JobType type, jb::core::JsonValue const& payload, SecretProvider& provider)
    -> jb::core::Result<jb::core::JsonValue, PayloadPreparationFailure>;

} // namespace jb::jobu::detail
