#pragma once

#include "io_device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace jb::net {

/// TCP socket connection state.
enum class SocketState : std::uint8_t {
    Unconnected, ///< No socket connection exists
    Connecting,  ///< A nonblocking connection attempt is in progress
    Connected,   ///< The socket is connected
    Closing,     ///< The socket is flushing queued writes before closing
};

/// Event-loop-driven client TCP socket.
///
/// TcpSocket is a byte-oriented IODevice for client TCP connections. The v1 API
/// accepts numeric addresses only; DNS resolution and server/listener support
/// are intentionally outside this class.
class TcpSocket : public jb::core::IODevice {
public:

    /// Constructs an unconnected TCP socket.
    /// @param[in] parent Optional parent that owns this socket
    explicit TcpSocket(jb::core::Object* parent = nullptr);

    /// Closes the socket if needed.
    ~TcpSocket() override;

    /// Starts a nonblocking connection to a numeric address and port.
    void connect_to_host(std::string_view address, std::uint16_t port);

    /// Flushes queued writes and then closes the socket.
    void disconnect_from_host();

    /// Closes immediately and clears any buffered data.
    void abort();

    /// Returns the current socket state.
    [[nodiscard]] auto state() const noexcept -> SocketState;

    /// Returns the peer address passed to connect_to_host().
    [[nodiscard]] auto peer_address() const noexcept -> std::string const&;

    /// Returns the peer port passed to connect_to_host().
    [[nodiscard]] auto peer_port() const noexcept -> std::uint16_t;

    /// Returns true when a socket fd is open.
    [[nodiscard]] auto is_open() const -> bool override;

    /// Equivalent to disconnect_from_host().
    void close() override;

    /// Reads up to @p max_size buffered bytes.
    [[nodiscard]] auto read(std::size_t max_size) -> std::string override;

    /// Reads all buffered bytes.
    [[nodiscard]] auto read_all() -> std::string override;

    /// Reads one buffered text line without a trailing LF or CRLF delimiter.
    [[nodiscard]] auto read_line(std::size_t max_size = static_cast<std::size_t>(-1)) -> std::string override;

    /// Returns true when a complete buffered line is available.
    [[nodiscard]] auto can_read_line() const -> bool override;

    /// Queues bytes for writing.
    /// @return Number of bytes accepted for writing
    auto write(std::string_view data) -> std::size_t override;

    /// Returns the number of buffered bytes available to read.
    [[nodiscard]] auto bytes_available() const -> std::size_t override;

    /// Emitted when the socket connection succeeds.
    jb::core::Signal<> connected;

    /// Emitted when the socket disconnects.
    jb::core::Signal<> disconnected;

private:

    void close_socket(bool emit_disconnected);
    void handle_fd_event(jb::core::FdEvents events);
    void handle_connect_ready();
    void read_available();
    void write_pending();
    void set_state(SocketState state);
    void update_watch();
};

} // namespace jb::net
