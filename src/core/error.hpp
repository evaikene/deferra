/** @file error.hpp
 * @brief Defines the project-owned error value used across public module boundaries.
 */
#pragma once

#include <cstdint>
#include <string>

namespace jb::core {

/// Broad categories used for generic control flow and future RPC mapping.
enum class ErrorCategory : std::uint8_t {
    InvalidArgument,
    NotFound,
    Conflict,
    PermissionDenied,
    Unavailable,
    ResourceExhausted,
    Cancelled,
    Timeout,
    Io,
    Unsupported,
    Internal,
};

/// Stable, dependency-independent description of an operational failure.
/// Use `code` for program decisions and `message` for users and logs. No field may contain sensitive values.
struct Error {
    /// Broad category for generic failure handling.
    ErrorCategory category{ErrorCategory::Internal};
    /// Stable module-owned machine identifier.
    std::string   code;
    /// User-safe explanatory text.
    std::string   message;
    /// Optional backend diagnostic that callers must not depend on.
    std::string   detail;

    /// Compares every error field for equality.
    auto operator==(Error const&) const -> bool = default;
};

} // namespace jb::core
