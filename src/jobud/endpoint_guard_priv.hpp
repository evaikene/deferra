#pragma once

#include "error.hpp"
#include "result.hpp"
#include "runtime_paths_priv.hpp"

#include <functional>
#include <sys/socket.h>
#include <sys/types.h>

namespace jb::jobud::detail {

/// Synchronous private syscall seam. It follows connect's return/errno contract and is never retained.
using EndpointConnect = std::function<int(int, sockaddr const*, socklen_t)>;

/// Owns one endpoint lock under final credentials. The prepared, protected runtime parent is borrowed
/// and must outlive this guard. Keep the guard through listener and database teardown. Its lock pathname
/// is never unlinked: removing it would let another daemon lock a different inode at the same endpoint.
class EndpointGuard {
public:
    EndpointGuard(EndpointGuard const&)                        = delete;
    auto operator=(EndpointGuard const&) -> EndpointGuard&     = delete;
    EndpointGuard(EndpointGuard&&) noexcept                    = default;
    auto operator=(EndpointGuard&&) noexcept -> EndpointGuard& = default;

    /// Opens a no-follow, owner-only regular lock file and takes nonblocking exclusive ownership.
    /// Call after permanent identity verification and before opening the database. Existing unsafe
    /// entries are preserved; neither acquisition failure nor destruction removes the lock file.
    [[nodiscard]] static auto acquire(TrustedParent const& parent, uid_t user)
        -> jb::core::Result<EndpointGuard, jb::core::Error>;

    /// Rechecks the lock's named inode and the protected parent before admission.
    [[nodiscard]] auto verify() const -> jb::core::Result<void, jb::core::Error>;

    /// Call only after exclusive database ownership succeeds, before recovery or dispatch.
    /// Removes only an unchanged, final-user-owned socket whose nonblocking connect definitively
    /// refuses. Live, in-progress, interrupted, backlog-full and unexpected outcomes preserve it.
    /// The optional connect strategy is a synchronous test seam; production uses the native syscall.
    [[nodiscard]] auto prepare_socket(EndpointConnect const& connect = {}) const
        -> jb::core::Result<void, jb::core::Error>;

private:
    EndpointGuard(TrustedParent const& parent, uid_t user, PathDescriptor lock, dev_t device, ino_t inode);

    TrustedParent const* _parent;
    uid_t                _user;
    PathDescriptor       _lock;
    dev_t                _device;
    ino_t                _inode;
};

} // namespace jb::jobud::detail
