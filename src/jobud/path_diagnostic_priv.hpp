#pragma once

#include "error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::jobud::detail {

enum class PathRole : std::uint8_t {
    Configuration,
    StateDirectory,
    RuntimeDirectory,
    DatabaseArtifact
};

enum class PathOperation : std::uint8_t {
    Open,
    Inspect,
    SetOwnership,
    SetPermissions
};

/// Owning snapshot from the operation which refused a path. Missing facts stay absent.
/// configured_path names the original resource; failing_path names the component actually checked.
struct PathDiagnostic {
    PathRole                                  role;
    std::filesystem::path                     configured_path;
    std::filesystem::path                     failing_path;
    std::optional<unsigned>                   actual_mode;
    std::optional<unsigned>                   required_mode;
    std::optional<std::uint64_t>              actual_uid;
    std::optional<std::uint64_t>              required_uid;
    std::optional<std::uint64_t>              actual_gid;
    std::optional<std::uint64_t>              required_gid;
    std::optional<int>                        system_errno;
    /// Retain the inspected kind so collision advice never needs another pathname lookup.
    std::optional<std::filesystem::file_type> actual_type;
    /// Some stable reason tokens cover several syscalls; retain which operation actually failed.
    std::optional<PathOperation>              operation;
};

/// Preserves the existing error contract. Account-resolution failures have no path diagnostic.
struct PathFailure {
    jb::core::Error               error;
    std::optional<PathDiagnostic> diagnostic;
};

/// Copies native stat mode/identity values; does not inspect the filesystem or read errno.
void set_path_metadata(PathDiagnostic& diagnostic, unsigned native_mode, std::uint64_t uid, std::uint64_t gid);

struct PathDiagnosticField {
    std::string_view                                       name;
    std::variant<std::string, std::int64_t, std::uint64_t> value;
};

/// All strings own their storage, including fields later borrowed by the synchronous logger.
struct FormattedPathFailure {
    std::string                      message;
    std::string                      action;
    std::vector<PathDiagnosticField> fields;
};

/// Formats only captured facts and reviewed fixed reasons. Never forwards backend message/detail text.
[[nodiscard]] auto format_path_failure(std::string_view subsystem, PathFailure const& failure) -> FormattedPathFailure;

/// One safe stderr line without its trailing newline, including code and corrective advice.
/// Controls and malformed UTF-8 are escaped before the logger exists.
[[nodiscard]] auto path_failure_text(PathFailure const& failure) -> std::string;

/// Emits jobud.failed with an independent human message and safe typed fields.
void log_path_failure(std::string_view     subsystem,
                      PathFailure const&   failure,
                      std::source_location location = std::source_location::current());

} // namespace jb::jobud::detail
