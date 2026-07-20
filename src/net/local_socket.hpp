/**
 * @file local_socket.hpp
 * @brief Provides an event-loop-driven local filesystem byte-stream device.
 */
#pragma once

#include "io_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace jb::net {

/// Local socket connection state.
enum class LocalSocketState : std::uint8_t {
    Unconnected, ///< No local socket lifecycle is active
    Connecting,  ///< A nonblocking connection attempt is in progress
    Connected,   ///< The local socket is connected
    Closing,     ///< Queued writes are being flushed before closing
};

/// Platform-neutral credentials reported for a connected local peer.
struct LocalPeerCredentials {
    /// Numeric operating-system process identifier, or absent when unavailable.
    std::optional<std::uint64_t> process_id;
    /// Numeric operating-system user identifier, or absent when unavailable.
    std::optional<std::uint64_t> user_id;
    /// Numeric operating-system group identifier, or absent when unavailable.
    std::optional<std::uint64_t> group_id;
};

class LocalServer;

/// Event-loop-driven client socket for local filesystem IPC.
///
/// LocalSocket is a binary, byte-oriented IODevice. It owns its native socket
/// and, like other Object-derived event-loop types, all operations must run in
/// the object's event-loop thread. Passing a parent transfers object lifetime
/// ownership to that parent.
///
/// Connect signal handlers should be installed before connect_to_server() is
/// called because an immediately successful connection may emit connected
/// synchronously. Every open lifecycle ends with the inherited closed signal;
/// connected lifecycles emit disconnected immediately before closed.
/// A socket returned by LocalServer is already Connected and does not emit
/// connected retroactively; inspect it or install later lifecycle handlers
/// immediately after taking ownership.
class LocalSocket final : public jb::core::IODevice {
public:

    /// Constructs an unconnected local socket.
    /// @param[in] parent Optional parent that owns this socket
    explicit LocalSocket(jb::core::Object* parent = nullptr);

    /// Releases native resources and buffered data without emitting lifecycle signals.
    ~LocalSocket() override;

    /// Starts a nonblocking connection to a filesystem Unix-domain socket.
    ///
    /// Empty paths, paths containing NUL bytes, abstract-namespace addresses,
    /// and paths too long for the native filesystem socket address are rejected.
    /// Starting a new attempt aborts any active lifecycle and clears its buffers,
    /// error, path, and credentials.
    /// @param[in] path Filesystem path of the local server
    void connect_to_server(std::filesystem::path const& path);

    /// Flushes queued output and then closes the socket.
    ///
    /// A connection attempt that has not completed is cancelled immediately.
    /// Buffered input remains available after a graceful disconnect.
    void disconnect_from_server();

    /// Immediately closes the socket and discards buffered input and output.
    ///
    /// An active lifecycle emits disconnected followed by the inherited closed
    /// signal. Calling abort() while unconnected has no effect.
    void abort();

    /// Returns the current local socket lifecycle state.
    [[nodiscard]] auto state() const noexcept -> LocalSocketState;

    /// Returns the most recently attempted server path.
    ///
    /// A successful path remains available after ordinary disconnection and is
    /// cleared when the next connection attempt starts.
    [[nodiscard]] auto server_path() const noexcept -> std::filesystem::path const&;

    /// Returns the credentials from the most recently successful connection.
    ///
    /// Credentials remain available after ordinary disconnection and are
    /// cleared when the next connection attempt starts.
    [[nodiscard]] auto peer_credentials() const noexcept -> LocalPeerCredentials const&;

    /// Sets the maximum number of bytes buffered for reading.
    ///
    /// Zero means unlimited. Lowering the limit below the current occupancy does
    /// not discard data; socket reads remain paused until capacity is available.
    /// @param[in] bytes Maximum buffered byte count, or zero for unlimited
    void set_read_buffer_limit(std::size_t bytes);

    /// Returns the configured input-buffer limit, where zero means unlimited.
    [[nodiscard]] auto read_buffer_limit() const noexcept -> std::size_t;

    /// Returns true while connecting, connected, or flushing a graceful close.
    [[nodiscard]] auto is_open() const -> bool override;

    /// Equivalent to disconnect_from_server().
    void close() override;

    /// Removes and returns up to @p max_size buffered bytes.
    /// @param[in] max_size Maximum number of bytes to remove
    /// @return The removed bytes, preserving embedded NUL values
    [[nodiscard]] auto read(std::size_t max_size) -> std::string override;

    /// Removes and returns all currently buffered bytes.
    /// @return All buffered bytes, preserving embedded NUL values
    [[nodiscard]] auto read_all() -> std::string override;

    /// Removes and returns one buffered line without its LF or CRLF delimiter.
    ///
    /// If @p max_size is reached before LF, that prefix is returned. Incomplete
    /// lines otherwise remain buffered while connected; after disconnection,
    /// remaining bytes form a final readable line.
    /// @param[in] max_size Maximum number of bytes to remove
    /// @return The available line or prefix, or an empty string when incomplete
    [[nodiscard]] auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string override;

    /// Returns true when read_line() can return a complete or final buffered line.
    [[nodiscard]] auto can_read_line() const -> bool override;

    /// Accepts bytes into the ordered nonblocking output queue.
    ///
    /// Writes are queued while connecting and extend an in-progress graceful
    /// close. Empty writes have no effect.
    /// @param[in] data Bytes to queue, including any embedded NUL values
    /// @return Number of bytes accepted into the output queue
    auto write(std::string_view data) -> std::size_t override;

    /// Returns the number of bytes currently buffered for reading.
    [[nodiscard]] auto bytes_available() const -> std::size_t override;

    /// Emitted after credentials are stored and the state becomes Connected.
    ///
    /// This signal precedes any ready_read notification for the new lifecycle.
    jb::core::Signal<> connected;

    /// Emitted when an established lifecycle disconnects or an attempt is cancelled.
    ///
    /// The signal immediately precedes closed. For peer EOF with newly received
    /// bytes, ready_read is emitted first. For post-connect failures,
    /// disconnected precedes error_occurred and closed. A failed connection
    /// attempt emits error_occurred and closed without disconnected.
    jb::core::Signal<> disconnected;

private:
    friend class LocalServer;
    struct Private;
    explicit LocalSocket(Private& dd, jb::core::Object* parent = nullptr);
};

} // namespace jb::net
