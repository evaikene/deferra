#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "job.hpp"
#include "json.hpp"
#include "management.hpp"
#include "result.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

/// Owns the original editable projection, independently of the fetched definition and registry.
/// Only const access is exposed so draft edits cannot change the captured identity, revision, or comparison values.
class JobEditBaseline final {
public:
    /// Projects a definition using public JSON encoders, preserving concrete schedules and payload contents.
    /// Returns a fixed, input-safe error if the definition cannot be encoded.
    [[nodiscard]] static auto from_job(jb::jobu::JobDefinition const& job, jb::jobu::AttributeRegistry const& registry)
        -> jb::core::Result<JobEditBaseline, jb::core::Error>;

    /// Returns the eight-member update-shaped projection, borrowed for this baseline's lifetime.
    [[nodiscard]] auto value() const -> jb::core::JsonValue const& { return _value; }

private:
    explicit JobEditBaseline(jb::core::JsonValue value);

    jb::core::JsonValue _value;
};

/// Owns exactly the minimal params and their shared-decoder result for later recovery-file writing and submission.
struct PreparedJobUpdate {
    /// Original identity/revision and only changed writable values.
    jb::core::JsonValue        params;
    /// Owning typed request decoded from params through the shared command boundary.
    jb::jobu::UpdateJobRequest request;
};

/// Prepends editor-visible // instructions to pretty JSON, bounding the complete JSONC draft to 16 MiB.
/// The result is an interactive draft; request-file input still requires strict JSON.
[[nodiscard]] auto format_job_edit_draft(jb::core::JsonValue const& value)
    -> jb::core::Result<std::string, jb::core::Error>;

/// Parses at most 16 MiB of edited draft text through the shared JSONC parser and validates retained fields.
/// Identity/revision are mandatory and fixed. Omitted fields/attributes mean no update; payload/schedule changes
/// replace the whole value. Validation uses the existing update codecs, leaving domain admission to the server.
/// Returns an empty optional for a no-op, or owning minimal params and the shared decoder's typed update.
/// Neither input nor registry is retained. Performs no I/O or rendering, and errors never contain draft contents.
[[nodiscard]] auto
prepare_job_update(JobEditBaseline const& baseline, std::string_view draft, jb::jobu::AttributeRegistry const& registry)
    -> jb::core::Result<std::optional<PreparedJobUpdate>, jb::core::Error>;

} // namespace jb::jobuctl::detail
