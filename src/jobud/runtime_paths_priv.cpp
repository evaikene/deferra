#include "runtime_paths_priv.hpp"

#include "privileges_priv.hpp"
#include "startup_priv.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace jb::jobud::detail {
namespace {

using PathResult = jb::core::Result<std::optional<TrustedParent>, PathFailure>;
using VoidResult = jb::core::Result<void, PathFailure>;

auto unsafe(char const* reason, PathDiagnostic diagnostic) -> PathFailure
{
    return {
        .error      = {.category = jb::core::ErrorCategory::PermissionDenied,
                       .code     = "jobud.path.unsafe",
                       .message  = "Daemon path violates the protected filesystem policy",
                       .detail   = reason},
        .diagnostic = std::move(diagnostic)
    };
}

auto inspect_failed(char const*                  reason,
                    int                          saved_errno,
                    PathDiagnostic const&        context,
                    std::optional<PathOperation> operation = std::nullopt) -> PathFailure
{
    auto diagnostic         = context;
    diagnostic.system_errno = saved_errno;
    diagnostic.operation    = operation;
    if (saved_errno == EACCES || saved_errno == EPERM || saved_errno == ELOOP) {
        return unsafe(reason, std::move(diagnostic));
    }
    return {
        .error      = {.category = jb::core::ErrorCategory::Io,
                       .code     = "jobud.path.inspect_failed",
                       .message  = "Daemon path operation failed",
                       .detail   = reason},
        .diagnostic = std::move(diagnostic)
    };
}

auto path_context(PathRole                     role,
                  std::filesystem::path const& configured,
                  std::filesystem::path const& failing,
                  uid_t                        user) -> PathDiagnostic
{
    return {.role = role, .configured_path = configured, .failing_path = failing, .required_uid = user};
}

auto observed(PathDiagnostic diagnostic, struct stat const& metadata) -> PathDiagnostic
{
    set_path_metadata(diagnostic, metadata.st_mode, metadata.st_uid, metadata.st_gid);
    return diagnostic;
}

auto trusted_owner(struct stat const& metadata, uid_t user) -> bool
{
    return metadata.st_uid == 0 || metadata.st_uid == user;
}

auto shared_sticky(struct stat const& metadata) -> bool
{
    return (metadata.st_mode & S_ISVTX) != 0 && (metadata.st_mode & 0022) != 0;
}

auto validate_ancestor(struct stat const&   metadata,
                       uid_t                user,
                       std::optional<gid_t> traversal_group,
                       PathDiagnostic       diagnostic) -> VoidResult
{
    diagnostic = observed(std::move(diagnostic), metadata);
    if (!S_ISDIR(metadata.st_mode) || !trusted_owner(metadata, user)) {
        return VoidResult::failure(unsafe("parent_owner_or_type", diagnostic));
    }
    if ((metadata.st_mode & 0022) != 0 && !shared_sticky(metadata)) {
        return VoidResult::failure(unsafe("parent_writable", diagnostic));
    }
    if (traversal_group && (metadata.st_mode & 0001) == 0 &&
        (metadata.st_gid != *traversal_group || (metadata.st_mode & 0010) == 0)) {
        diagnostic.required_gid = *traversal_group;
        return VoidResult::failure(unsafe("runtime_parent_not_traversable", std::move(diagnostic)));
    }
    return VoidResult::success();
}

void prepend_components(std::deque<std::string>& pending, std::filesystem::path const& path)
{
    std::vector<std::string> components;
    for (auto const& part : path.relative_path()) {
        if (!part.empty() && part != ".") {
            components.push_back(part.string());
        }
    }
    for (auto const& component : components | std::views::reverse) {
        pending.push_front(component);
    }
}

struct Directory {
    PathDescriptor        descriptor;
    std::filesystem::path path;
    struct stat           metadata{};
};

struct LeafCreation {
    gid_t  group;
    mode_t mode;
};

auto walk_parent(std::filesystem::path const& path,
                 PathRole                     role,
                 std::filesystem::path const& configured_path,
                 uid_t                        user,
                 bool                         allow_missing,
                 std::optional<LeafCreation>  creation,
                 std::optional<gid_t>         traversal_group = std::nullopt) -> PathResult
{
    auto diagnostic = path_context(role, configured_path, path, user);
    if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        return PathResult::failure(unsafe("invalid_leaf", diagnostic));
    }

    // Walk from retained directory descriptors. Symlink targets enter this same walk, so neither
    // their controlling chain nor traversal through '..' can disappear in lexical normalization.
    diagnostic.failing_path = "/";
    Directory root{.descriptor = PathDescriptor{}, .path = "/"};
    root.descriptor = PathDescriptor{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (root.descriptor.get() < 0) {
        return PathResult::failure(inspect_failed("root_open", errno, diagnostic, PathOperation::Open));
    }
    if (::fstat(root.descriptor.get(), &root.metadata) != 0) {
        return PathResult::failure(inspect_failed("root_open", errno, diagnostic, PathOperation::Inspect));
    }
    auto root_trust = validate_ancestor(root.metadata, user, traversal_group, diagnostic);
    if (!root_trust) {
        return PathResult::failure(std::move(root_trust).error());
    }

    std::vector<Directory> directories;
    directories.push_back(std::move(root));
    std::deque<std::string> pending;
    prepend_components(pending, path.parent_path());
    unsigned symlinks{0};

    while (!pending.empty()) {
        auto component = std::move(pending.front());
        pending.pop_front();
        if (component == "..") {
            if (directories.size() > 1) {
                directories.pop_back();
            }
            continue;
        }

        diagnostic            = path_context(role, configured_path, directories.back().path / component, user);
        auto const  parent_fd = directories.back().descriptor.get();
        struct stat metadata{};
        bool        created{false};
        if (::fstatat(parent_fd, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) {
                return PathResult::failure(inspect_failed("parent_inspect", errno, diagnostic));
            }
            if (!creation || !pending.empty()) {
                if (allow_missing) {
                    return PathResult::success(std::nullopt);
                }
                return PathResult::failure(inspect_failed("missing_parent", errno, diagnostic));
            }

            // Only this named leaf may be created. Never manufacture missing ancestors or repair a collision.
            if (::mkdirat(parent_fd, component.c_str(), 0700) == 0) {
                created = true;
            }
            else if (errno != EEXIST) {
                return PathResult::failure(inspect_failed("leaf_create", errno, diagnostic));
            }
            if (::fstatat(parent_fd, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
                return PathResult::failure(inspect_failed("leaf_inspect", errno, diagnostic));
            }
        }

        diagnostic = observed(std::move(diagnostic), metadata);
        if (S_ISLNK(metadata.st_mode)) {
            // Only aliases controlled by the trusted owners may redirect this walk. A sticky
            // shared directory is crossed through a real protected child, never through an alias.
            if ((creation && pending.empty()) || !trusted_owner(metadata, user) ||
                shared_sticky(directories.back().metadata) || ++symlinks > 40) {
                return PathResult::failure(unsafe("parent_symlink", observed(diagnostic, metadata)));
            }
            std::array<char, 4096> target{};
            auto const             size = ::readlinkat(parent_fd, component.c_str(), target.data(), target.size());
            if (size < 0) {
                return PathResult::failure(inspect_failed("alias_read", errno, diagnostic));
            }
            if (static_cast<std::size_t>(size) == target.size()) {
                return PathResult::failure(unsafe("alias_too_long", observed(diagnostic, metadata)));
            }
            auto const alias = std::filesystem::path{
                std::string{target.data(), static_cast<std::size_t>(size)}
            };
            if (alias.is_absolute()) {
                directories.resize(1);
            }
            prepend_components(pending, alias);
            continue;
        }
        if (!S_ISDIR(metadata.st_mode)) {
            return PathResult::failure(unsafe("parent_owner_or_type", observed(diagnostic, metadata)));
        }

        // Pin and compare the inspected inode before using it as the next controlling parent.
        Directory directory{.descriptor = PathDescriptor{}, .path = diagnostic.failing_path};
        directory.descriptor =
            PathDescriptor{::openat(parent_fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (directory.descriptor.get() < 0) {
            return PathResult::failure(inspect_failed("directory_open", errno, diagnostic, PathOperation::Open));
        }
        if (::fstat(directory.descriptor.get(), &directory.metadata) != 0) {
            return PathResult::failure(inspect_failed("directory_open", errno, diagnostic, PathOperation::Inspect));
        }
        if (directory.metadata.st_dev != metadata.st_dev || directory.metadata.st_ino != metadata.st_ino) {
            return PathResult::failure(unsafe("parent_replaced", observed(diagnostic, directory.metadata)));
        }

        if (created) {
            // Ownership changes can invalidate the earlier snapshot. Do not report those old
            // identities/modes as current when a later setup operation fails.
            diagnostic               = path_context(role, configured_path, directory.path, user);
            diagnostic.required_mode = creation->mode;
            diagnostic.required_gid  = creation->group;
            if (::fchown(directory.descriptor.get(), user, creation->group) != 0) {
                return PathResult::failure(
                    inspect_failed("leaf_ownership", errno, diagnostic, PathOperation::SetOwnership));
            }
            if (::fchmod(directory.descriptor.get(), creation->mode) != 0) {
                return PathResult::failure(
                    inspect_failed("leaf_ownership", errno, diagnostic, PathOperation::SetPermissions));
            }
            if (::fstat(directory.descriptor.get(), &directory.metadata) != 0) {
                return PathResult::failure(inspect_failed("leaf_ownership", errno, diagnostic, PathOperation::Inspect));
            }
        }
        auto trust = validate_ancestor(directory.metadata, user, traversal_group, diagnostic);
        if (!trust) {
            return PathResult::failure(std::move(trust).error());
        }
        directories.push_back(std::move(directory));
    }

    auto& parent = directories.back();
    if (shared_sticky(parent.metadata)) {
        return PathResult::failure(
            unsafe("shared_parent_requires_private_leaf",
                   observed(path_context(role, configured_path, parent.path, user), parent.metadata)));
    }
    return PathResult::success(TrustedParent{.directory       = std::move(parent.descriptor),
                                             .path            = std::move(parent.path),
                                             .leaf            = path.filename(),
                                             .role            = role,
                                             .configured_path = configured_path});
}

auto validate_leaf(TrustedParent const& parent, uid_t user, gid_t group, mode_t mode) -> VoidResult
{
    auto diagnostic          = path_context(parent.role, parent.configured_path, parent.path, user);
    diagnostic.required_mode = mode;
    if (mode == 0750) {
        diagnostic.required_gid = group;
    }
    struct stat metadata{};
    if (::fstat(parent.directory.get(), &metadata) != 0) {
        return VoidResult::failure(inspect_failed("leaf_stat", errno, diagnostic));
    }
    if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != user || (metadata.st_mode & 07777) != mode ||
        (mode == 0750 && metadata.st_gid != group)) {
        return VoidResult::failure(unsafe("leaf_owner_or_mode", observed(std::move(diagnostic), metadata)));
    }
    return VoidResult::success();
}

auto same_directory(TrustedParent const& held, uid_t user, std::optional<gid_t> traversal_group) -> VoidResult
{
    auto reopened =
        walk_parent(held.path / held.leaf, held.role, held.configured_path, user, false, std::nullopt, traversal_group);
    if (!reopened) {
        return VoidResult::failure(std::move(reopened).error());
    }
    auto        diagnostic = path_context(held.role, held.configured_path, held.path, user);
    struct stat original{};
    struct stat current{};
    if (::fstat(held.directory.get(), &original) != 0 || ::fstat(reopened->value().directory.get(), &current) != 0) {
        return VoidResult::failure(inspect_failed("parent_recheck", errno, diagnostic));
    }
    if (original.st_dev != current.st_dev || original.st_ino != current.st_ino) {
        return VoidResult::failure(unsafe("parent_replaced", observed(std::move(diagnostic), current)));
    }
    return VoidResult::success();
}

} // namespace

PathDescriptor::PathDescriptor(int descriptor) noexcept
    : _descriptor{descriptor}
{}

PathDescriptor::~PathDescriptor()
{
    if (_descriptor >= 0) {
        static_cast<void>(::close(_descriptor));
    }
}

PathDescriptor::PathDescriptor(PathDescriptor&& other) noexcept
    : _descriptor{std::exchange(other._descriptor, -1)}
{}

auto PathDescriptor::operator=(PathDescriptor&& other) noexcept -> PathDescriptor&
{
    if (this != &other) {
        if (_descriptor >= 0) {
            static_cast<void>(::close(_descriptor));
        }
        _descriptor = std::exchange(other._descriptor, -1);
    }
    return *this;
}

auto open_trusted_parent(std::filesystem::path const& path, uid_t trusted_user, bool allow_missing, PathRole role)
    -> PathResult
{
    return walk_parent(path, role, path, trusted_user, allow_missing, std::nullopt);
}

auto PreparedRuntimePaths::database_path() const -> std::filesystem::path
{
    return state.path / state.leaf;
}

auto PreparedRuntimePaths::socket_path() const -> std::filesystem::path
{
    return runtime.path / runtime.leaf;
}

auto prepare_runtime_paths(StartupOptions const& options,
                           FinalIdentity const&  identity,
                           PrivilegeOperations&  operations) -> jb::core::Result<PreparedRuntimePaths, PathFailure>
{
    using PrepareResult = jb::core::Result<PreparedRuntimePaths, PathFailure>;

    auto       group = identity.group;
    auto const mode  = static_cast<mode_t>(options.socket_mode == 0660 ? 0750 : 0700);
    if (options.socket_group) {
        auto resolved = operations.group(*options.socket_group);
        if (!resolved) {
            return PrepareResult::failure({.error = std::move(resolved).error()});
        }
        group = *resolved;
    }
    if (group != identity.group &&
        std::ranges::find(identity.supplementary_groups, group) == identity.supplementary_groups.end()) {
        return PrepareResult::failure(unsafe("runtime_group_not_authorized",
                                             {.role            = PathRole::RuntimeDirectory,
                                              .configured_path = options.socket_path,
                                              .failing_path    = options.socket_path.parent_path(),
                                              .required_uid    = identity.user,
                                              .required_gid    = group}));
    }

    auto state = walk_parent(options.database_path,
                             PathRole::StateDirectory,
                             options.database_path,
                             identity.user,
                             false,
                             LeafCreation{.group = identity.group, .mode = 0700});
    if (!state) {
        return PrepareResult::failure(std::move(state).error());
    }
    auto state_trust = validate_leaf(state->value(), identity.user, identity.group, 0700);
    if (!state_trust) {
        return PrepareResult::failure(std::move(state_trust).error());
    }
    auto const traversal_group = mode == 0750 ? std::optional{group} : std::nullopt;
    auto       runtime         = walk_parent(options.socket_path,
                                             PathRole::RuntimeDirectory,
                                             options.socket_path,
                                             identity.user,
                                             false,
                                             LeafCreation{.group = group, .mode = mode},
                                             traversal_group);
    if (!runtime) {
        return PrepareResult::failure(std::move(runtime).error());
    }
    auto runtime_trust = validate_leaf(runtime->value(), identity.user, group, mode);
    if (!runtime_trust) {
        return PrepareResult::failure(std::move(runtime_trust).error());
    }
    sockaddr_un address{};
    if ((runtime->value().path / runtime->value().leaf).native().size() >= sizeof(address.sun_path)) {
        return PrepareResult::failure(unsafe("resolved_socket_path_too_long",
                                             path_context(PathRole::RuntimeDirectory,
                                                          options.socket_path,
                                                          runtime->value().path / runtime->value().leaf,
                                                          identity.user)));
    }
    return PrepareResult::success({.state         = std::move(state->value()),
                                   .runtime       = std::move(runtime->value()),
                                   .runtime_group = group,
                                   .runtime_mode  = mode});
}

auto verify_runtime_paths(PreparedRuntimePaths const& paths, uid_t user) -> VoidResult
{
    auto same = same_directory(paths.state, user, std::nullopt);
    if (!same) {
        return same;
    }
    auto const traversal_group = paths.runtime_mode == 0750 ? std::optional{paths.runtime_group} : std::nullopt;
    same                       = same_directory(paths.runtime, user, traversal_group);
    if (!same) {
        return same;
    }
    auto state = validate_leaf(paths.state, user, 0, 0700);
    if (!state) {
        return state;
    }
    return validate_leaf(paths.runtime, user, paths.runtime_group, paths.runtime_mode);
}

auto validate_database_artifacts(PreparedRuntimePaths const& paths, uid_t user) -> VoidResult
{
    // SQLite opens its own files by pathname. Protected parents exclude other-user replacement;
    // same-UID processes already share daemon authority. Never follow or repair an existing artifact.
    for (auto const* suffix : {"", ".lock", "-wal", "-shm", "-journal"}) {
        auto const name = paths.state.leaf.string() + suffix;
        auto       diagnostic =
            path_context(PathRole::DatabaseArtifact, paths.state.configured_path, paths.state.path / name, user);
        struct stat metadata{};
        if (::fstatat(paths.state.directory.get(), name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            return VoidResult::failure(inspect_failed("artifact_inspect", errno, diagnostic));
        }
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid != user || (metadata.st_mode & 07177) != 0) {
            return VoidResult::failure(
                unsafe("artifact_owner_type_or_mode", observed(std::move(diagnostic), metadata)));
        }
    }
    return VoidResult::success();
}

} // namespace jb::jobud::detail
