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

using PathResult = jb::core::Result<std::optional<TrustedParent>, jb::core::Error>;
using VoidResult = jb::core::Result<void, jb::core::Error>;

auto unsafe(char const* reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::PermissionDenied,
            .code     = "jobud.path.unsafe",
            .message  = "Daemon path violates the protected filesystem policy",
            .detail   = reason};
}

auto inspect_failed(char const* reason) -> jb::core::Error
{
    if (errno == EACCES || errno == EPERM || errno == ELOOP) {
        return unsafe(reason);
    }
    return {.category = jb::core::ErrorCategory::Io,
            .code     = "jobud.path.inspect_failed",
            .message  = "Daemon path operation failed",
            .detail   = reason};
}

auto trusted_owner(struct stat const& metadata, uid_t user) -> bool
{
    return metadata.st_uid == 0 || metadata.st_uid == user;
}

auto shared_sticky(struct stat const& metadata) -> bool
{
    return (metadata.st_mode & S_ISVTX) != 0 && (metadata.st_mode & 0022) != 0;
}

auto validate_ancestor(struct stat const& metadata, uid_t user, std::optional<gid_t> traversal_group) -> VoidResult
{
    if (!S_ISDIR(metadata.st_mode) || !trusted_owner(metadata, user)) {
        return VoidResult::failure(unsafe("parent_owner_or_type"));
    }
    if ((metadata.st_mode & 0022) != 0 && !shared_sticky(metadata)) {
        return VoidResult::failure(unsafe("parent_writable"));
    }
    if (traversal_group && (metadata.st_mode & 0001) == 0 &&
        (metadata.st_gid != *traversal_group || (metadata.st_mode & 0010) == 0)) {
        return VoidResult::failure(unsafe("runtime_parent_not_traversable"));
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
                 uid_t                        user,
                 bool                         allow_missing,
                 std::optional<LeafCreation>  creation,
                 std::optional<gid_t>         traversal_group = std::nullopt) -> PathResult
{
    if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..") {
        return PathResult::failure(unsafe("invalid_leaf"));
    }

    // Walk from retained directory descriptors. Symlink targets enter this same walk, so neither
    // their controlling chain nor traversal through '..' can disappear in lexical normalization.
    Directory root{.descriptor = PathDescriptor{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)},
                   .path       = "/"};
    if (root.descriptor.get() < 0 || ::fstat(root.descriptor.get(), &root.metadata) != 0) {
        return PathResult::failure(inspect_failed("root_open"));
    }
    auto root_trust = validate_ancestor(root.metadata, user, traversal_group);
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

        auto const  parent_fd = directories.back().descriptor.get();
        struct stat metadata{};
        bool        created{false};
        if (::fstatat(parent_fd, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) {
                return PathResult::failure(inspect_failed("parent_inspect"));
            }
            if (!creation || !pending.empty()) {
                if (allow_missing) {
                    return PathResult::success(std::nullopt);
                }
                return PathResult::failure(inspect_failed("missing_parent"));
            }

            // Only this named leaf may be created. Never manufacture missing ancestors or repair a collision.
            if (::mkdirat(parent_fd, component.c_str(), 0700) == 0) {
                created = true;
            }
            else if (errno != EEXIST) {
                return PathResult::failure(inspect_failed("leaf_create"));
            }
            if (::fstatat(parent_fd, component.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
                return PathResult::failure(inspect_failed("leaf_inspect"));
            }
        }

        if (S_ISLNK(metadata.st_mode)) {
            // Only aliases controlled by the trusted owners may redirect this walk. A sticky
            // shared directory is crossed through a real protected child, never through an alias.
            if ((creation && pending.empty()) || !trusted_owner(metadata, user) ||
                shared_sticky(directories.back().metadata) || ++symlinks > 40) {
                return PathResult::failure(unsafe("parent_symlink"));
            }
            std::array<char, 4096> target{};
            auto const             size = ::readlinkat(parent_fd, component.c_str(), target.data(), target.size());
            if (size < 0) {
                return PathResult::failure(inspect_failed("alias_read"));
            }
            if (static_cast<std::size_t>(size) == target.size()) {
                return PathResult::failure(unsafe("alias_too_long"));
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
            return PathResult::failure(unsafe("parent_owner_or_type"));
        }

        // Pin and compare the inspected inode before using it as the next controlling parent.
        Directory directory{
            .descriptor =
                PathDescriptor{::openat(parent_fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)},
            .path = directories.back().path / component};
        if (directory.descriptor.get() < 0 || ::fstat(directory.descriptor.get(), &directory.metadata) != 0) {
            return PathResult::failure(inspect_failed("directory_open"));
        }
        if (directory.metadata.st_dev != metadata.st_dev || directory.metadata.st_ino != metadata.st_ino) {
            return PathResult::failure(unsafe("parent_replaced"));
        }

        if (created) {
            if (::fchown(directory.descriptor.get(), user, creation->group) != 0 ||
                ::fchmod(directory.descriptor.get(), creation->mode) != 0 ||
                ::fstat(directory.descriptor.get(), &directory.metadata) != 0) {
                return PathResult::failure(inspect_failed("leaf_ownership"));
            }
        }
        auto trust = validate_ancestor(directory.metadata, user, traversal_group);
        if (!trust) {
            return PathResult::failure(std::move(trust).error());
        }
        directories.push_back(std::move(directory));
    }

    auto& parent = directories.back();
    if (shared_sticky(parent.metadata)) {
        return PathResult::failure(unsafe("shared_parent_requires_private_leaf"));
    }
    return PathResult::success(TrustedParent{.directory = std::move(parent.descriptor),
                                             .path      = std::move(parent.path),
                                             .leaf      = path.filename()});
}

auto validate_leaf(TrustedParent const& parent, uid_t user, gid_t group, mode_t mode) -> VoidResult
{
    struct stat metadata{};
    if (::fstat(parent.directory.get(), &metadata) != 0) {
        return VoidResult::failure(inspect_failed("leaf_stat"));
    }
    if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != user || (metadata.st_mode & 07777) != mode ||
        (mode == 0750 && metadata.st_gid != group)) {
        return VoidResult::failure(unsafe("leaf_owner_or_mode"));
    }
    return VoidResult::success();
}

auto same_directory(TrustedParent const& held, uid_t user, std::optional<gid_t> traversal_group) -> VoidResult
{
    auto reopened = walk_parent(held.path / held.leaf, user, false, std::nullopt, traversal_group);
    if (!reopened) {
        return VoidResult::failure(std::move(reopened).error());
    }
    struct stat original{};
    struct stat current{};
    if (::fstat(held.directory.get(), &original) != 0 || ::fstat(reopened->value().directory.get(), &current) != 0) {
        return VoidResult::failure(inspect_failed("parent_recheck"));
    }
    if (original.st_dev != current.st_dev || original.st_ino != current.st_ino) {
        return VoidResult::failure(unsafe("parent_replaced"));
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

auto open_trusted_parent(std::filesystem::path const& path, uid_t trusted_user, bool allow_missing) -> PathResult
{
    return walk_parent(path, trusted_user, allow_missing, std::nullopt);
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
                           PrivilegeOperations&  operations) -> jb::core::Result<PreparedRuntimePaths, jb::core::Error>
{
    using PrepareResult = jb::core::Result<PreparedRuntimePaths, jb::core::Error>;

    auto       group = identity.group;
    auto const mode  = static_cast<mode_t>(options.socket_mode == 0660 ? 0750 : 0700);
    if (options.socket_group) {
        auto resolved = operations.group(*options.socket_group);
        if (!resolved) {
            return PrepareResult::failure(std::move(resolved).error());
        }
        group = *resolved;
    }
    if (group != identity.group &&
        std::ranges::find(identity.supplementary_groups, group) == identity.supplementary_groups.end()) {
        return PrepareResult::failure(unsafe("runtime_group_not_authorized"));
    }

    auto state =
        walk_parent(options.database_path, identity.user, false, LeafCreation{.group = identity.group, .mode = 0700});
    if (!state) {
        return PrepareResult::failure(std::move(state).error());
    }
    auto state_trust = validate_leaf(state->value(), identity.user, identity.group, 0700);
    if (!state_trust) {
        return PrepareResult::failure(std::move(state_trust).error());
    }
    auto const traversal_group = mode == 0750 ? std::optional{group} : std::nullopt;
    auto       runtime         = walk_parent(options.socket_path,
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
        return PrepareResult::failure(unsafe("resolved_socket_path_too_long"));
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
        auto const  name = paths.state.leaf.string() + suffix;
        struct stat metadata{};
        if (::fstatat(paths.state.directory.get(), name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue;
            }
            return VoidResult::failure(inspect_failed("artifact_inspect"));
        }
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid != user || (metadata.st_mode & 07177) != 0) {
            return VoidResult::failure(unsafe("artifact_owner_type_or_mode"));
        }
    }
    return VoidResult::success();
}

} // namespace jb::jobud::detail
