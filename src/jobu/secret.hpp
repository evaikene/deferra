/** @file secret.hpp
 * @brief Defines the public metadata-only view of a JobU secret.
 */
#pragma once

#include "time_source.hpp"

#include <string>

namespace jb::jobu {

/** Public metadata for a named secret.
 *
 * Secret bytes are intentionally absent and remain private repository input/output. They must not appear in public
 * read values, errors, logs, RPC results, or command output.
 */
struct SecretMetadata {
    /// Canonical secret name.
    std::string            name;
    /// Initial creation time in UTC.
    jb::core::UtcTimePoint created_at;
    /// Time the secret value was most recently replaced, in UTC.
    jb::core::UtcTimePoint updated_at;
};

} // namespace jb::jobu
