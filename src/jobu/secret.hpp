/// @file secret.hpp
/// @brief Defines the public metadata-only view of a JobU secret.
///
#pragma once

#include "byte_buffer.hpp"
#include "time_source.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace jb::jobu {

/// Public metadata for a named secret.
///
/// Secret bytes are intentionally absent. Administrative reads and generated errors/logs never expose their
/// bytes, length, digest or preview; trusted execution lookup is a separate private capability.
///
struct SecretMetadata {
    /// Canonical secret name.
    std::string            name;
    /// Initial creation time in UTC.
    jb::core::UtcTimePoint created_at;
    /// Time the secret value was most recently replaced, in UTC.
    jb::core::UtcTimePoint updated_at;
};

/// Owning input for one secret upsert; bytes are consumed without text decoding or normalization.
struct SetSecretRequest {
    /// Canonical identifier of at most 128 bytes, using the existing lowercase attribute-name grammar.
    std::string          name;
    /// Zero through 65,536 arbitrary bytes. An empty value is valid.
    jb::core::ByteBuffer value;
};

/// Live metadata page in ascending canonical-name order.
struct SecretListRequest {
    /// Maximum returned items, from 1 through 200.
    std::size_t                limit{100};
    /// Exclusive canonical-name boundary. The named secret need not still exist.
    std::optional<std::string> after_name;
};

/// Metadata-only page; concurrent changes between calls may change the remaining live view.
struct SecretPage {
    std::vector<SecretMetadata> items;
    /// Last emitted name when another row exists, otherwise absent.
    std::optional<std::string>  next_after_name;
};

} // namespace jb::jobu
