#pragma once

#include "secret_provider.hpp"

#include <catch2/catch_test_macros.hpp>

namespace jb::test {

/// Literal-only fixtures must never reach the secret lookup boundary.
class RejectingSecretProvider final : public jb::jobu::SecretProvider {
public:
    auto resolve(std::string_view /*name*/) -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error> override
    {
        FAIL_CHECK("A literal-only fixture unexpectedly resolved a secret");
        return jb::core::Result<jb::core::ByteBuffer, jb::core::Error>::failure({
            .category = jb::core::ErrorCategory::Internal,
            .code     = "test.secret.unexpected_lookup",
            .message  = "Unexpected secret lookup",
        });
    }
};

} // namespace jb::test
