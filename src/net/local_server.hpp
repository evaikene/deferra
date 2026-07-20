/**
 * @file local_server.hpp
 * @brief Provides an event-loop-driven server for local filesystem byte streams.
 */
#pragma once

#include "io_device.hpp"
#include "local_socket.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace jb::net {

/// Configuration applied when a LocalServer starts listening.
struct LocalServerOptions {
    /// Permission bits applied to the socket entry before listening begins.
    ///
    /// All values in std::filesystem::perms::mask, including perms::none, are
    /// valid. perms::unknown and values containing other bits are rejected.
    std::filesystem::perms permissions{std::filesystem::perms::owner_read | std::filesystem::perms::owner_write};

    /// Positive native listen backlog passed to the operating system.
    int backlog{128};

    /// Positive maximum number of accepted sockets retained for callers.
    std::size_t max_pending_connections{64};

    /// Per-connection input-buffer limit in bytes; zero means unlimited.
    std::size_t accepted_read_buffer_limit{2U * 1024U * 1024U};
};

/// Event-loop-driven listener for local filesystem IPC connections.
///
/// LocalServer owns its listener and pending connections. Like other
/// Object-derived event-loop types, it is thread-affine and all operations must
/// run in its event-loop thread. Passing a parent transfers server lifetime
/// ownership to that parent.
class LocalServer final : public jb::core::Object {
public:
    /// Constructs a non-listening local server.
    /// @param[in] parent Optional parent that owns this server and supplies its event-loop affinity
    explicit LocalServer(jb::core::Object* parent = nullptr);

    /// Releases the listener, pending sockets, and owned filesystem entry without emitting signals.
    ~LocalServer() override;

    /// Starts listening at a new filesystem Unix-domain socket path.
    ///
    /// The path and options are validated before native resources are retained.
    /// The parent directory must already exist, and any existing entry at @p path
    /// is preserved and causes failure; stale entries are never removed. After a
    /// successful bind, the requested permissions are applied before listening
    /// begins. A synchronous failure returns false, stores an error, and does not
    /// emit accept_error. Calling listen() while already listening leaves the
    /// active listener, path, and pending queue unchanged.
    /// @param[in] path Filesystem path for the local server
    /// @param[in] options Listener, queue, permission, and accepted-buffer options
    /// @return True when the listener and its event-loop watch are active
    [[nodiscard]] auto listen(std::filesystem::path const& path, LocalServerOptions options = {}) -> bool;

    /// Stops listening and destroys pending, not-yet-transferred connections.
    ///
    /// Already transferred sockets remain usable. The socket entry is removed
    /// only when it is still a socket with the device and inode created by this
    /// server instance. Repeated calls after resources are released are no-ops.
    void close();

    /// Returns true while the native listener and event-loop watch lifecycle is active.
    [[nodiscard]] auto is_listening() const noexcept -> bool;

    /// Returns the most recently attempted server path.
    ///
    /// The referenced path remains valid until this server is destroyed or the
    /// next non-active listen attempt replaces it; successful close does not clear it.
    [[nodiscard]] auto server_path() const noexcept -> std::filesystem::path const&;

    /// Returns the number of accepted sockets waiting for ownership transfer.
    [[nodiscard]] auto pending_connection_count() const noexcept -> std::size_t;

    /// Transfers ownership of the oldest pending connection to the caller.
    ///
    /// A server-accepted LocalSocket is already connected and watched. Taking a
    /// connection can rearm paused acceptance; the event loop performs any next
    /// accept asynchronously.
    /// @return The oldest pending socket, or null when the queue is empty
    [[nodiscard]] auto take_next_connection() -> std::unique_ptr<LocalSocket>;

    /// Returns the last listen, accept, or cleanup error category.
    ///
    /// A successful listen clears the error. Otherwise it remains available
    /// until another operation changes it.
    [[nodiscard]] auto error() const noexcept -> jb::core::IOError;

    /// Returns the last listen, accept, or cleanup error message.
    ///
    /// The referenced string remains valid until this server is destroyed or a
    /// later operation changes the stored error.
    [[nodiscard]] auto error_string() const noexcept -> std::string const&;

    /// Emitted once after an accept drain queues one or more connections.
    ///
    /// One emission may represent several sockets. Consumers must drain
    /// take_next_connection() until it returns null.
    jb::core::Signal<> new_connection;

    /// Emitted for an asynchronous accept or accepted-socket adoption failure.
    ///
    /// The arguments are the stored error category and message. Connections
    /// already present in the pending queue remain valid and ordered.
    jb::core::Signal<jb::core::IOError, std::string> accept_error;

private:
    struct Private;
};

} // namespace jb::net
