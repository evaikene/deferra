#pragma once

#include "error.hpp"
#include "job.hpp"
#include "json.hpp"
#include "result.hpp"

namespace jb::jobu::detail {

/// Validates newly supplied payloads, returning owned JSON with an explicit canonical HTTP method.
/// Takes ownership or copies its argument; other members and CLI payloads retain their values.
/// Checks the normalized document's existing size, template, and structural limits without resolving secrets.
/// Stored-data readers and metadata-only updates must retain the separate compatibility validation path.
[[nodiscard]] auto normalize_submitted_job_payload(JobType type, jb::core::JsonValue payload)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

} // namespace jb::jobu::detail
