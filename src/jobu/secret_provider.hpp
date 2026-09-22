/// @file secret_provider.hpp
/// @brief Trusted synchronous access to named execution secrets.
#pragma once

#include "byte_buffer.hpp"
#include "error.hpp"
#include "result.hpp"

#include <string_view>

namespace jb::jobu {

/// Grants trusted runner orchestration access to secret bytes; not a public value-read endpoint.
/// Calls are synchronous on the owner's thread, without callbacks or event processing. An asynchronous or external
/// provider needs a separate lifecycle design. Providers and their borrowed dependencies must outlive their consumers.
class SecretProvider {
public:
    virtual ~SecretProvider() = default;

    /// Resolves a canonical name borrowed only for this call; never retains caller references.
    /// Returns owning bytes (0..65,536), with an empty value distinct from a missing name.
    /// Represented failures are jobu.secret.not_found, db.* storage errors, or jobu.storage.* persisted-data errors
    /// (excluding invalid_limit). Codes must be trusted machine identifiers; orchestration treats other failures as
    /// fatal provider-contract failures, never as ordinary invalid secret values.
    [[nodiscard]] virtual auto resolve(std::string_view name)
        -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error> = 0;
};

} // namespace jb::jobu
