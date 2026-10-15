#include "path_diagnostic_priv.hpp"

#include "logging.hpp"
#include "text_validation.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <sys/stat.h>
#include <type_traits>

namespace jb::jobud::detail {
namespace {

auto resource_name(PathRole role) -> std::string_view
{
    switch (role) {
        case PathRole::Configuration:
            return "configuration";
        case PathRole::StateDirectory:
            return "state_directory";
        case PathRole::RuntimeDirectory:
            return "runtime_directory";
        case PathRole::DatabaseArtifact:
            return "database_artifact";
    }
    return "path";
}

auto resource_label(PathRole role) -> std::string_view
{
    switch (role) {
        case PathRole::Configuration:
            return "Configuration file";
        case PathRole::StateDirectory:
            return "State directory";
        case PathRole::RuntimeDirectory:
            return "Runtime directory";
        case PathRole::DatabaseArtifact:
            return "Database artifact";
    }
    return "Path";
}

auto entry_kind(std::optional<std::filesystem::file_type> type) -> std::string_view
{
    using enum std::filesystem::file_type;
    if (!type) {
        return "entry of unknown type";
    }
    switch (*type) {
        case regular:
            return "regular file";
        case directory:
            return "directory";
        case symlink:
            return "symbolic link";
        case fifo:
            return "FIFO";
        case socket:
            return "socket";
        default:
            return "nonregular entry";
    }
}

auto reviewed_reason(PathFailure const& failure) -> std::string_view
{
    // A private carrier may also wrap account/backend failures. Neither their messages nor arbitrary
    // detail strings become log fields merely because a caller used the path overload.
    static constexpr auto reasons = std::to_array<std::string_view>({"invalid_leaf",
                                                                     "root_open",
                                                                     "parent_inspect",
                                                                     "missing_parent",
                                                                     "leaf_create",
                                                                     "leaf_inspect",
                                                                     "parent_symlink",
                                                                     "alias_read",
                                                                     "alias_too_long",
                                                                     "parent_owner_or_type",
                                                                     "directory_open",
                                                                     "parent_replaced",
                                                                     "leaf_ownership",
                                                                     "parent_writable",
                                                                     "runtime_parent_not_traversable",
                                                                     "shared_parent_requires_private_leaf",
                                                                     "leaf_stat",
                                                                     "leaf_owner_or_mode",
                                                                     "parent_recheck",
                                                                     "runtime_group_not_authorized",
                                                                     "resolved_socket_path_too_long",
                                                                     "artifact_inspect",
                                                                     "artifact_owner_type_or_mode",
                                                                     "config_open",
                                                                     "config_stat",
                                                                     "config_owner_type_or_mode",
                                                                     "config_read",
                                                                     "runtime_parent_inspect",
                                                                     "endpoint_runtime_parent_changed",
                                                                     "socket_probe_path",
                                                                     "probe_socket",
                                                                     "probe_connected",
                                                                     "probe_ambiguous",
                                                                     "probe_connect",
                                                                     "endpoint_lock_owner_type_or_mode",
                                                                     "lock_inspect",
                                                                     "lock_open",
                                                                     "lock_descriptor",
                                                                     "lock_held",
                                                                     "lock_acquire",
                                                                     "lock_verify",
                                                                     "endpoint_lock_changed",
                                                                     "socket_inspect",
                                                                     "endpoint_socket_owner_or_type",
                                                                     "stale_socket_recheck",
                                                                     "endpoint_socket_changed",
                                                                     "stale_socket_remove"});
    auto const*           found   = std::ranges::find(reasons, failure.error.detail);
    return found == reasons.end() ? "unknown" : *found;
}

auto operation_name(std::optional<PathOperation> operation, std::string_view reason) -> std::string_view
{
    if (operation) {
        switch (*operation) {
            case PathOperation::Open:
                return "open";
            case PathOperation::Inspect:
                return "inspect";
            case PathOperation::SetOwnership:
                return "set directory ownership on";
            case PathOperation::SetPermissions:
                return "set directory permissions on";
        }
    }
    if (reason == "root_open" || reason == "directory_open" || reason == "config_open") {
        return "open";
    }
    if (reason == "leaf_create") {
        return "create directory";
    }
    if (reason == "leaf_ownership") {
        return "set directory ownership/permissions";
    }
    if (reason == "alias_read") {
        return "read symbolic link";
    }
    if (reason == "config_read") {
        return "read configuration";
    }
    return "inspect";
}

auto mode_text(unsigned mode) -> std::string
{
    return fmt::format("{:04o}", mode);
}

auto utf8_width(unsigned char byte) -> std::size_t
{
    if (byte < 0x80U) {
        return 1;
    }
    if (byte >= 0xc2U && byte <= 0xdfU) {
        return 2;
    }
    if (byte >= 0xe0U && byte <= 0xefU) {
        return 3;
    }
    if (byte >= 0xf0U && byte <= 0xf4U) {
        return 4;
    }
    return 0;
}

auto identity_text(std::optional<std::uint64_t> value) -> std::string
{
    return value ? fmt::format("{}", *value) : "unknown";
}

auto observed_mode(std::optional<unsigned> mode) -> std::string
{
    return mode ? mode_text(*mode) : "unknown";
}

auto safe_line(std::string_view text) -> std::string
{
    std::string result;
    for (std::size_t index = 0; index < text.size();) {
        auto const byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x20U || byte == 0x7fU) {
            result += fmt::format("\\x{:02x}", byte);
            ++index;
            continue;
        }
        auto const width = utf8_width(byte);
        if (width == 0 || width > text.size() - index || !jb::core::is_valid_utf8(text.substr(index, width))) {
            result += fmt::format("\\x{:02x}", byte);
            ++index;
            continue;
        }
        auto const scalar = text.substr(index, width);
        // C1 controls and Unicode line separators are unsafe in terminal diagnostics as well.
        if ((width == 2 && byte == 0xc2U && static_cast<unsigned char>(text[index + 1]) <= 0x9fU) ||
            scalar == "\xe2\x80\xa8" || scalar == "\xe2\x80\xa9") {
            for (auto const part : scalar) {
                result += fmt::format("\\x{:02x}", static_cast<unsigned char>(part));
            }
        }
        else {
            result.append(scalar);
        }
        index += width;
    }
    return result;
}

void explain(PathFailure const& failure, std::string_view reason, FormattedPathFailure& output)
{
    auto const& diagnostic = *failure.diagnostic;
    auto const  path       = safe_line(diagnostic.failing_path.string());
    auto const  label      = resource_label(diagnostic.role);
    auto const  owner_wrong =
        diagnostic.actual_uid && diagnostic.required_uid && *diagnostic.actual_uid != *diagnostic.required_uid;
    auto const group_wrong =
        diagnostic.actual_gid && diagnostic.required_gid && *diagnostic.actual_gid != *diagnostic.required_gid;

    if (reason == "missing_parent") {
        output.message = fmt::format("Directory '{}' is missing; {} depends on it.", path, label);
        output.action  = "Provision the missing parent directory for this custom path; JobU creates only the final "
                         "operational directory.";
    }
    else if (reason == "runtime_parent_not_traversable") {
        output.message =
            fmt::format("Directory '{}' blocks administrative group traversal (GID {}); actual GID {}, permissions {}.",
                        path,
                        identity_text(diagnostic.required_gid),
                        identity_text(diagnostic.actual_gid),
                        observed_mode(diagnostic.actual_mode));
        output.action = "Provide search (execute) access for the authorized administrative group along the runtime "
                        "path; do not grant group write access.";
    }
    else if (reason == "runtime_group_not_authorized") {
        output.message =
            fmt::format("Runtime directory '{}' requires GID {}, but daemon UID {} is not authorized for that group.",
                        path,
                        identity_text(diagnostic.required_gid),
                        identity_text(diagnostic.required_uid));
        output.action =
            "Select an authorized socket group or provision the daemon account's group membership before restarting.";
    }
    else if (reason == "parent_writable") {
        output.message = fmt::format(
            "Ancestor '{}' has permissions {}; group/other write access lets others control the protected {} path.",
            path,
            observed_mode(diagnostic.actual_mode),
            resource_name(diagnostic.role));
        output.action = "Remove group/other write access from this inspected ancestor, or choose a protected path.";
    }
    else if (reason == "parent_symlink" ||
             (reason == "config_open" && diagnostic.actual_type == std::filesystem::file_type::symlink) ||
             (diagnostic.actual_type &&
              ((reason == "parent_owner_or_type" && *diagnostic.actual_type != std::filesystem::file_type::directory) ||
               ((reason == "artifact_owner_type_or_mode" || reason == "config_owner_type_or_mode") &&
                *diagnostic.actual_type != std::filesystem::file_type::regular)))) {
        output.message =
            fmt::format("{} '{}' is a {}; JobU preserved the entry.", label, path, entry_kind(diagnostic.actual_type));
        auto const* required_type =
            reason == "parent_symlink" || reason == "parent_owner_or_type" ? "directory" : "regular file";
        output.action = fmt::format("Choose a path with the required {} type and a trusted parent chain; inspect the "
                                    "collision before making changes.",
                                    required_type);
    }
    else if (diagnostic.system_errno) {
        output.message = fmt::format("Unable to {} '{}' for {} (errno {}).",
                                     operation_name(diagnostic.operation, reason),
                                     path,
                                     resource_name(diagnostic.role),
                                     *diagnostic.system_errno);
        output.action  = "Check search access on the protected parent chain and the invoking/final daemon identity's "
                         "access required by this operation.";
    }
    else if (owner_wrong || group_wrong) {
        output.message =
            fmt::format("{} '{}' has UID {} and GID {}; required identity is UID {}{}.",
                        label,
                        path,
                        identity_text(diagnostic.actual_uid),
                        identity_text(diagnostic.actual_gid),
                        identity_text(diagnostic.required_uid),
                        diagnostic.required_gid ? fmt::format(" and GID {}", *diagnostic.required_gid) : std::string{});
        if (reason == "parent_owner_or_type" || diagnostic.role == PathRole::Configuration) {
            output.message += " Root ownership (UID 0) is also accepted for this protected path.";
        }
        output.action = "Arrange ownership for the intended daemon identity (the invoking identity for configuration); "
                        "changing permissions alone is insufficient.";
    }
    else if (reason == "leaf_owner_or_mode" && diagnostic.actual_mode && diagnostic.required_mode) {
        output.message = fmt::format("{} '{}' has permissions {}; required permissions are {}.",
                                     label,
                                     path,
                                     mode_text(*diagnostic.actual_mode),
                                     mode_text(*diagnostic.required_mode));
        output.action = fmt::format("Set permissions {} on this exact, inspected directory owned by the daemon UID {}.",
                                    mode_text(*diagnostic.required_mode),
                                    identity_text(diagnostic.required_uid));
    }
    else if (reason == "artifact_owner_type_or_mode") {
        output.message = fmt::format("Database artifact '{}' has permissions {}; required is a daemon-owned private "
                                     "regular file with at most owner read/write access (0600), without special bits.",
                                     path,
                                     observed_mode(diagnostic.actual_mode));
        output.action  = "Correct the ownership and private-file permissions of this exact artifact before restarting; "
                         "JobU preserved it.";
    }
    else if (reason == "config_owner_type_or_mode") {
        output.message =
            fmt::format("Configuration file '{}' has permissions {}; group/other write access is not allowed.",
                        path,
                        observed_mode(diagnostic.actual_mode));
        output.action =
            "Protect this regular file from group/other writes and keep it owned by the invoking user or root.";
    }
    else if (reason == "parent_replaced" || reason == "endpoint_runtime_parent_changed") {
        output.message =
            fmt::format("Protected directory '{}' changed after inspection; JobU refused the replacement.", path);
        output.action = "Restore a stable, correctly owned protected directory before restarting.";
    }
    else {
        output.message = fmt::format("{} '{}' violates the protected filesystem policy ({}).", label, path, reason);
        output.action  = "Use an absolute resource path under stable, trusted parents with the required ownership and "
                         "private access.";
    }
    if (diagnostic.configured_path != diagnostic.failing_path) {
        output.message += fmt::format(" Configured resource: '{}'.", safe_line(diagnostic.configured_path.string()));
    }
}

} // namespace

void set_path_metadata(PathDiagnostic& diagnostic, unsigned native_mode, std::uint64_t uid, std::uint64_t gid)
{
    diagnostic.actual_mode = native_mode & 07777U;
    diagnostic.actual_uid  = uid;
    diagnostic.actual_gid  = gid;
    using enum std::filesystem::file_type;
    if (S_ISDIR(native_mode)) {
        diagnostic.actual_type = directory;
    }
    else if (S_ISREG(native_mode)) {
        diagnostic.actual_type = regular;
    }
    else if (S_ISLNK(native_mode)) {
        diagnostic.actual_type = symlink;
    }
    else if (S_ISFIFO(native_mode)) {
        diagnostic.actual_type = fifo;
    }
    else if (S_ISSOCK(native_mode)) {
        diagnostic.actual_type = socket;
    }
    else {
        diagnostic.actual_type = unknown;
    }
}

auto format_path_failure(std::string_view subsystem, PathFailure const& failure) -> FormattedPathFailure
{
    FormattedPathFailure output{.message = "Daemon startup failed."};
    output.fields.push_back({.name = "code", .value = failure.error.code});
    output.fields.push_back({.name = "subsystem", .value = std::string{subsystem}});
    if (!failure.diagnostic) {
        auto const reason = reviewed_reason(failure);
        if (reason != "unknown") {
            output.fields.push_back({.name = "reason", .value = std::string{reason}});
        }
        return output;
    }

    auto const  reason     = reviewed_reason(failure);
    auto const& diagnostic = *failure.diagnostic;
    explain(failure, reason, output);
    output.fields.push_back({.name = "reason", .value = std::string{reason}});
    output.fields.push_back({.name = "resource", .value = std::string{resource_name(diagnostic.role)}});
    output.fields.push_back({.name = "path", .value = safe_line(diagnostic.failing_path.string())});
    if (diagnostic.configured_path != diagnostic.failing_path) {
        output.fields.push_back({.name = "configured_path", .value = safe_line(diagnostic.configured_path.string())});
    }
    if (diagnostic.actual_mode) {
        output.fields.push_back({.name = "actual_mode", .value = mode_text(*diagnostic.actual_mode)});
    }
    if (diagnostic.required_mode) {
        output.fields.push_back({.name = "required_mode", .value = mode_text(*diagnostic.required_mode)});
    }
    if (diagnostic.actual_uid) {
        output.fields.push_back({.name = "actual_uid", .value = *diagnostic.actual_uid});
    }
    if (diagnostic.required_uid) {
        output.fields.push_back({.name = "required_uid", .value = *diagnostic.required_uid});
    }
    if (diagnostic.actual_gid) {
        output.fields.push_back({.name = "actual_gid", .value = *diagnostic.actual_gid});
    }
    if (diagnostic.required_gid) {
        output.fields.push_back({.name = "required_gid", .value = *diagnostic.required_gid});
    }
    if (diagnostic.system_errno) {
        output.fields.push_back({.name = "errno", .value = static_cast<std::int64_t>(*diagnostic.system_errno)});
    }
    output.fields.push_back({.name = "action", .value = output.action});
    return output;
}

auto path_failure_text(PathFailure const& failure) -> std::string
{
    auto const formatted = format_path_failure("configuration", failure);
    return fmt::format("{} Action: {} ({})", formatted.message, formatted.action, safe_line(failure.error.code));
}

void log_path_failure(std::string_view subsystem, PathFailure const& failure, std::source_location location)
{
    auto const                      formatted = format_path_failure(subsystem, failure);
    std::vector<jb::core::LogField> fields;
    fields.reserve(formatted.fields.size());
    for (auto const& field : formatted.fields) {
        std::visit(
            [&](auto const& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::string>) {
                    fields.push_back({.name = field.name, .value = std::string_view{value}});
                }
                else {
                    fields.push_back({.name = field.name, .value = value});
                }
            },
            field.value);
    }
    // Every field view borrows formatted until the synchronous facade/sink returns.
    jb::core::log_event_message(jb::core::LogLevel::Error, "jobud.failed", formatted.message, fields, location);
}

} // namespace jb::jobud::detail
