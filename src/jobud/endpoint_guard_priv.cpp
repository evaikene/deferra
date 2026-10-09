#include "endpoint_guard_priv.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace jb::jobud::detail {
namespace {

using VoidResult = jb::core::Result<void, jb::core::Error>;

auto unsafe(char const* reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::PermissionDenied,
            .code     = "jobud.path.unsafe",
            .message  = "Daemon endpoint violates the protected filesystem policy",
            .detail   = reason};
}

auto inspect_failed(char const* reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Io,
            .code     = "jobud.socket.inspect_failed",
            .message  = "Unable to establish safe daemon endpoint ownership",
            .detail   = reason};
}

auto in_use(char const* reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Conflict,
            .code     = "jobud.socket.in_use",
            .message  = "Daemon endpoint is already owned or may still be live",
            .detail   = reason};
}

auto valid_lock(struct stat const& metadata, uid_t user) -> bool
{
    return S_ISREG(metadata.st_mode) && metadata.st_uid == user && (metadata.st_mode & 07777) == 0600;
}

auto same_socket(struct stat const& before, struct stat const& after) -> bool
{
    return S_ISSOCK(after.st_mode) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_uid == after.st_uid;
}

auto verify_parent(TrustedParent const& parent, uid_t user) -> VoidResult
{
    struct stat held{};
    struct stat named{};
    if (::fstat(parent.directory.get(), &held) != 0 || ::lstat(parent.path.c_str(), &named) != 0) {
        return VoidResult::failure(inspect_failed("runtime_parent_inspect"));
    }
    if (!S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino || held.st_uid != user ||
        ((held.st_mode & 07777) != 0700 && (held.st_mode & 07777) != 0750)) {
        return VoidResult::failure(unsafe("endpoint_runtime_parent_changed"));
    }
    return VoidResult::success();
}

auto probe(std::filesystem::path const& path, EndpointConnect const& connect) -> VoidResult
{
    sockaddr_un address{};
    auto const& name = path.native();
    if (name.empty() || name.find('\0') != std::string::npos || name.size() >= sizeof(address.sun_path)) {
        return VoidResult::failure(unsafe("socket_probe_path"));
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1U);
    auto const length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1U);

    PathDescriptor socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (socket.get() < 0) {
        return VoidResult::failure(inspect_failed("probe_socket"));
    }

    // One nonblocking attempt is bounded without waiting on the owner thread. A pending or interrupted
    // attempt is ambiguous even if a later retry might refuse; only this definite refusal permits unlink.
    auto const* native_address = reinterpret_cast<sockaddr const*>(&address);
    auto const  result =
        connect ? connect(socket.get(), native_address, length) : ::connect(socket.get(), native_address, length);
    if (result == 0) {
        return VoidResult::failure(in_use("probe_connected"));
    }
    auto const error = errno;
    if (error == ECONNREFUSED) {
        return VoidResult::success();
    }
    if (error == EINPROGRESS || error == EALREADY || error == EAGAIN || error == EWOULDBLOCK || error == EINTR ||
        error == ETIMEDOUT) {
        return VoidResult::failure(in_use("probe_ambiguous"));
    }
    return VoidResult::failure(inspect_failed("probe_connect"));
}

} // namespace

EndpointGuard::EndpointGuard(TrustedParent const& parent, uid_t user, PathDescriptor lock, dev_t device, ino_t inode)
    : _parent{&parent}
    , _user{user}
    , _lock{std::move(lock)}
    , _device{device}
    , _inode{inode}
{}

auto EndpointGuard::acquire(TrustedParent const& parent, uid_t user) -> jb::core::Result<EndpointGuard, jb::core::Error>
{
    using GuardResult    = jb::core::Result<EndpointGuard, jb::core::Error>;
    auto parent_verified = verify_parent(parent, user);
    if (!parent_verified) {
        return GuardResult::failure(std::move(parent_verified).error());
    }
    auto const name = parent.leaf.string() + ".lock";

    struct stat existing{};
    if (::fstatat(parent.directory.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!valid_lock(existing, user)) {
            return GuardResult::failure(unsafe("endpoint_lock_owner_type_or_mode"));
        }
    }
    else if (errno != ENOENT) {
        return GuardResult::failure(inspect_failed("lock_inspect"));
    }

    // O_NONBLOCK also prevents a substituted FIFO from blocking before its type is checked.
    // Do not truncate or unlink a surviving lock file, including when flock reports another owner.
    PathDescriptor lock{
        ::openat(parent.directory.get(), name.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
    if (lock.get() < 0) {
        return GuardResult::failure(inspect_failed("lock_open"));
    }
    struct stat held{};
    if (::fstat(lock.get(), &held) != 0) {
        return GuardResult::failure(inspect_failed("lock_descriptor"));
    }
    if (!valid_lock(held, user)) {
        return GuardResult::failure(unsafe("endpoint_lock_owner_type_or_mode"));
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        auto const error = errno;
        return GuardResult::failure(error == EWOULDBLOCK || error == EAGAIN ? in_use("lock_held")
                                                                            : inspect_failed("lock_acquire"));
    }

    EndpointGuard guard{parent, user, std::move(lock), held.st_dev, held.st_ino};
    auto          verified = guard.verify();
    if (!verified) {
        return GuardResult::failure(std::move(verified).error());
    }
    return GuardResult::success(std::move(guard));
}

auto EndpointGuard::verify() const -> VoidResult
{
    // A lock on an unlinked or renamed inode cannot protect the endpoint's current pathname.
    auto parent_verified = verify_parent(*_parent, _user);
    if (!parent_verified) {
        return parent_verified;
    }

    auto const  name = _parent->leaf.string() + ".lock";
    struct stat held{};
    struct stat named{};
    if (::fstat(_lock.get(), &held) != 0 ||
        ::fstatat(_parent->directory.get(), name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
        return VoidResult::failure(inspect_failed("lock_verify"));
    }
    if (!valid_lock(held, _user) || !valid_lock(named, _user) || held.st_dev != _device || held.st_ino != _inode ||
        named.st_dev != _device || named.st_ino != _inode) {
        return VoidResult::failure(unsafe("endpoint_lock_changed"));
    }
    return VoidResult::success();
}

auto EndpointGuard::prepare_socket(EndpointConnect const& connect) const -> VoidResult
{
    auto verified = verify();
    if (!verified) {
        return verified;
    }

    auto const  name = _parent->leaf.string();
    struct stat existing{};
    if (::fstatat(_parent->directory.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? VoidResult::success() : VoidResult::failure(inspect_failed("socket_inspect"));
    }
    if (!S_ISSOCK(existing.st_mode) || existing.st_uid != _user) {
        return VoidResult::failure(unsafe("endpoint_socket_owner_or_type"));
    }

    auto refused = probe(_parent->path / _parent->leaf, connect);
    if (!refused) {
        return refused;
    }
    verified = verify();
    if (!verified) {
        return verified;
    }

    struct stat current{};
    if (::fstatat(_parent->directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return VoidResult::failure(inspect_failed("stale_socket_recheck"));
    }
    if (!same_socket(existing, current)) {
        return VoidResult::failure(unsafe("endpoint_socket_changed"));
    }
    if (::unlinkat(_parent->directory.get(), name.c_str(), 0) != 0) {
        return VoidResult::failure(inspect_failed("stale_socket_remove"));
    }
    return VoidResult::success();
}

} // namespace jb::jobud::detail
