#pragma once

#include "error.hpp"
#include "result.hpp"

#include <filesystem>
#include <optional>
#include <sys/types.h>

namespace jb::jobud::detail {

struct FinalIdentity;
class PrivilegeOperations;
struct StartupOptions;

/// Single-owner, close-on-exec descriptor. Moving transfers cleanup responsibility.
class PathDescriptor {
public:
    explicit PathDescriptor(int descriptor = -1) noexcept;
    ~PathDescriptor();
    PathDescriptor(PathDescriptor const&)                    = delete;
    auto operator=(PathDescriptor const&) -> PathDescriptor& = delete;
    PathDescriptor(PathDescriptor&& other) noexcept;
    auto operator=(PathDescriptor&& other) noexcept -> PathDescriptor&;

    [[nodiscard]] auto get() const noexcept -> int { return _descriptor; }

private:
    int _descriptor;
};

/// The inspected parent stays open while its leaf is opened or inspected relative to it.
struct TrustedParent {
    PathDescriptor        directory;
    std::filesystem::path path;
    std::filesystem::path leaf;
};

/// Resolves the original spelling, including symlink/.. traversal, through protected parents.
/// Root and trusted_user may control ancestors; shared sticky parents require a protected child.
/// allow_missing accepts absence only after the existing prefix has passed trust checks.
[[nodiscard]] auto open_trusted_parent(std::filesystem::path const& path, uid_t trusted_user, bool allow_missing)
    -> jb::core::Result<std::optional<TrustedParent>, jb::core::Error>;

/// Owns the verified directories until database and listener teardown. Paths have no unresolved aliases or dots.
struct PreparedRuntimePaths {
    TrustedParent state;
    TrustedParent runtime;
    gid_t         runtime_group{};
    mode_t        runtime_mode{0700};

    [[nodiscard]] auto database_path() const -> std::filesystem::path;
    [[nodiscard]] auto socket_path() const -> std::filesystem::path;
};

/// Prepares only explicit missing leaves under trusted existing parents, using the authorized final identity.
/// Existing trees are validated, never repaired. Call before applying identity and before resource construction.
[[nodiscard]] auto
prepare_runtime_paths(StartupOptions const& options, FinalIdentity const& identity, PrivilegeOperations& operations)
    -> jb::core::Result<PreparedRuntimePaths, jb::core::Error>;

/// Reopens names under the verified final credentials and checks they still identify the retained directories.
[[nodiscard]] auto verify_runtime_paths(PreparedRuntimePaths const& paths, uid_t user)
    -> jb::core::Result<void, jb::core::Error>;

/// Checks existing SQLite files without following leaf symlinks; absence permits later owner-only creation.
/// Invoke before open and after open/schema setup, before recovery or worker admission.
[[nodiscard]] auto validate_database_artifacts(PreparedRuntimePaths const& paths, uid_t user)
    -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::jobud::detail
