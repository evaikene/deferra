#include "tcp_socket.hpp"

#include "tcp_socket_priv.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jb::net {

namespace {

constexpr int         kInvalidFd{-1};
constexpr std::size_t kBufferSize{static_cast<const std::size_t>(16 * 1024)};

auto system_error_message(std::string_view context, int error) -> std::string
{
    return fmt::format("{}: {}", context, std::strerror(error));
}

auto set_nonblocking(int fd) -> bool
{
    auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto disable_sigpipe(int fd) -> bool
{
#if defined(SO_NOSIGPIPE)
    int yes = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0;
#else
    (void)fd;
    return true;
#endif
}

auto send_flags() -> int
{
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

auto make_sockaddr(std::string_view address, std::uint16_t port, sockaddr_storage& storage, socklen_t& length) -> int
{
    std::memset(&storage, 0, sizeof(storage));

    sockaddr_in addr4{};
    addr4.sin_family = AF_INET;
    addr4.sin_port   = htons(port);
    if (::inet_pton(AF_INET, std::string{address}.c_str(), &addr4.sin_addr) == 1) {
        std::memcpy(&storage, &addr4, sizeof(addr4));
        length = sizeof(addr4);
        return AF_INET;
    }

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port   = htons(port);
    if (::inet_pton(AF_INET6, std::string{address}.c_str(), &addr6.sin6_addr) == 1) {
        std::memcpy(&storage, &addr6, sizeof(addr6));
        length = sizeof(addr6);
        return AF_INET6;
    }

    return AF_UNSPEC;
}

void strip_crlf(std::string& line)
{
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
}

} // anonymous namespace

TcpSocket::TcpSocket(jb::core::Object* parent)
    : IODevice(*new priv::TcpSocketPrivate, parent)
{}

TcpSocket::~TcpSocket()
{
    release_socket();
}

void TcpSocket::connect_to_host(std::string_view address, std::uint16_t port)
{
    close_socket(false);

    auto* d = d_ptr<priv::TcpSocketPrivate>();
    d->input_buffer.clear();
    d->output_buffer.clear();
    clear_error();

    d->peer_address.assign(address);
    d->peer_port = port;

    auto* loop = event_loop();
    if (!loop) {
        set_error(jb::core::IOError::ResourceError, "socket requires an event loop");
        return;
    }

    sockaddr_storage storage{};
    socklen_t        length{};
    auto const       family = make_sockaddr(address, port, storage, length);
    if (family == AF_UNSPEC) {
        set_error(jb::core::IOError::InvalidArgument, "address must be a numeric IPv4 or IPv6 address");
        return;
    }

    auto fd = ::socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error(jb::core::IOError::OpenError, system_error_message("socket failed", errno));
        return;
    }

    if (!disable_sigpipe(fd)) {
        auto message = system_error_message("setsockopt failed", errno);
        ::close(fd);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    if (!set_nonblocking(fd)) {
        auto message = system_error_message("fcntl failed", errno);
        ::close(fd);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    d->fd         = fd;
    auto const rc = ::connect(fd, reinterpret_cast<sockaddr const*>(&storage), length);
    if (rc == 0) {
        set_state(SocketState::Connected);
        update_watch();
        emit(connected);
        return;
    }

    if (errno == EINPROGRESS) {
        set_state(SocketState::Connecting);
        update_watch();
        return;
    }

    auto message = system_error_message("connect failed", errno);
    fail_socket(jb::core::IOError::OpenError, std::move(message), false);
}

void TcpSocket::disconnect_from_host()
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    if (d->state == SocketState::Unconnected) {
        return;
    }

    if (d->output_buffer.empty()) {
        close_socket(true);
        return;
    }

    set_state(SocketState::Closing);
    update_watch();
    write_pending();
}

void TcpSocket::abort()
{
    auto*      d             = d_ptr<priv::TcpSocketPrivate>();
    auto const was_connected = d->state != SocketState::Unconnected;

    d->input_buffer.clear();
    d->output_buffer.clear();
    close_socket(was_connected);
}

auto TcpSocket::state() const noexcept -> SocketState
{
    return d_ptr<priv::TcpSocketPrivate const>()->state;
}

auto TcpSocket::peer_address() const noexcept -> std::string const&
{
    return d_ptr<priv::TcpSocketPrivate const>()->peer_address;
}

auto TcpSocket::peer_port() const noexcept -> std::uint16_t
{
    return d_ptr<priv::TcpSocketPrivate const>()->peer_port;
}

auto TcpSocket::is_open() const -> bool
{
    return d_ptr<priv::TcpSocketPrivate const>()->state != SocketState::Unconnected;
}

void TcpSocket::close()
{
    disconnect_from_host();
}

auto TcpSocket::read(std::size_t max_size) -> std::string
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    if (d->input_buffer.empty()) {
        return {};
    }

    clear_error();
    auto const size = std::min(max_size, d->input_buffer.size());
    auto       out  = d->input_buffer.substr(0, size);
    d->input_buffer.erase(0, size);

    return out;
}

auto TcpSocket::read_all() -> std::string
{
    clear_error();

    auto* d = d_ptr<priv::TcpSocketPrivate>();

    auto out = std::move(d->input_buffer);
    d->input_buffer.clear();

    return out;
}

auto TcpSocket::read_line(std::size_t max_size) -> std::string
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    auto const newline = d->input_buffer.find('\n');
    if (newline == std::string::npos && d->state != SocketState::Unconnected) {
        return {};
    }

    if (newline == std::string::npos && d->input_buffer.empty()) {
        return {};
    }

    clear_error();

    auto const has_newline = newline != std::string::npos && newline < max_size;
    auto const line_size   = has_newline ? newline + 1 : std::min(max_size, d->input_buffer.size());

    auto line = d->input_buffer.substr(0, line_size);
    d->input_buffer.erase(0, line_size);

    if (has_newline) {
        strip_crlf(line);
    }

    return line;
}

auto TcpSocket::can_read_line() const -> bool
{
    auto const* d = d_ptr<priv::TcpSocketPrivate const>();
    return d->input_buffer.find('\n') != std::string::npos ||
           (d->state == SocketState::Unconnected && !d->input_buffer.empty());
}

auto TcpSocket::write(std::string_view data) -> std::size_t
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    if (d->state == SocketState::Unconnected) {
        set_error(jb::core::IOError::NotOpen, "socket is not connected");
        return 0;
    }

    clear_error();
    d->output_buffer.append(data);
    update_watch();
    if (d->state == SocketState::Connected || d->state == SocketState::Closing) {
        write_pending();
    }

    return data.size();
}

auto TcpSocket::bytes_available() const -> std::size_t
{
    return d_ptr<priv::TcpSocketPrivate const>()->input_buffer.size();
}

void TcpSocket::set_state(SocketState state)
{
    d_ptr<priv::TcpSocketPrivate>()->state = state;
}

auto TcpSocket::release_socket() -> bool
{
    auto*      d        = d_ptr<priv::TcpSocketPrivate>();
    auto const was_open = d->state != SocketState::Unconnected;

    auto* loop = event_loop();
    if (loop && d->watch) {
        loop->unwatch_fd(d->watch);
    }
    d->watch = {};

    if (d->fd != kInvalidFd) {
        ::close(d->fd);
    }
    d->fd = kInvalidFd;
    set_state(SocketState::Unconnected);

    return was_open;
}

void TcpSocket::close_socket(bool emit_disconnected)
{
    auto const was_open = release_socket();

    if (emit_disconnected) {
        emit(disconnected);
    }
    if (was_open) {
        emit_closed();
    }
}

void TcpSocket::fail_socket(jb::core::IOError error, std::string message, bool emit_disconnected)
{
    auto const was_open = release_socket();

    if (emit_disconnected) {
        emit(disconnected);
    }
    set_error(error, std::move(message));
    if (was_open) {
        emit_closed();
    }
}

void TcpSocket::handle_fd_event(jb::core::FdEvents events)
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    if (d->state == SocketState::Unconnected) {
        return;
    }

    if (d->state == SocketState::Connecting && events.any()) {
        handle_connect_ready();
    }

    if (d->state == SocketState::Connected || d->state == SocketState::Closing) {
        if (events.test_any(jb::core::FdEvents{jb::core::FdEvent::Read})) {
            read_available();
        }
        if (d->state != SocketState::Unconnected && events.test_any(jb::core::FdEvents{jb::core::FdEvent::Write})) {
            write_pending();
        }
    }

    update_watch();
}

void TcpSocket::handle_connect_ready()
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    int       error = 0;
    socklen_t len   = sizeof(error);
    if (::getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        auto message = system_error_message("getsockopt failed", errno);
        fail_socket(jb::core::IOError::OpenError, std::move(message), false);
        return;
    }

    if (error != 0) {
        auto message = system_error_message("connect failed", error);
        fail_socket(jb::core::IOError::OpenError, std::move(message), false);
        return;
    }

    set_state(SocketState::Connected);
    emit(connected);
    write_pending();
}

void TcpSocket::read_available()
{
    bool read_any = false;

    auto* d = d_ptr<priv::TcpSocketPrivate>();

    for (;;) {
        char       buffer[kBufferSize];
        auto const n = ::recv(d->fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            d->input_buffer.append(buffer, static_cast<std::size_t>(n));
            read_any = true;
            continue;
        }

        if (n == 0) {
            if (read_any) {
                emit_ready_read();
            }
            close_socket(true);
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        auto message = system_error_message("recv failed", errno);
        if (read_any) {
            emit_ready_read();
        }
        fail_socket(jb::core::IOError::ReadError, std::move(message), true);
        return;
    }

    if (read_any) {
        emit_ready_read();
    }
}

void TcpSocket::write_pending()
{
    auto* d = d_ptr<priv::TcpSocketPrivate>();

    while (!d->output_buffer.empty()) {
        auto const n = ::send(d->fd, d->output_buffer.data(), d->output_buffer.size(), send_flags());
        if (n > 0) {
            auto const size = static_cast<std::size_t>(n);
            d->output_buffer.erase(0, size);
            emit_bytes_written(size);
            continue;
        }

        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        auto message = system_error_message("send failed", errno);
        fail_socket(jb::core::IOError::WriteError, std::move(message), true);
        return;
    }

    if (d->state == SocketState::Closing && d->output_buffer.empty()) {
        close_socket(true);
    }
}

void TcpSocket::update_watch()
{
    auto* d    = d_ptr<priv::TcpSocketPrivate>();
    auto* loop = event_loop();
    if (!loop || d->fd == kInvalidFd) {
        return;
    }

    jb::core::FdEvents events;
    if (d->state == SocketState::Connecting) {
        events.set(jb::core::FdEvent::Read);
        events.set(jb::core::FdEvent::Write);
    }
    if (d->state == SocketState::Connected || d->state == SocketState::Closing) {
        events.set(jb::core::FdEvent::Read);
        if (!d->output_buffer.empty() || d->state == SocketState::Closing) {
            events.set(jb::core::FdEvent::Write);
        }
    }

    if (events.none()) {
        if (d->watch) {
            loop->unwatch_fd(d->watch);
            d->watch = {};
        }
        return;
    }

    d->watch = loop->watch_fd(d->fd, events, [this](int, jb::core::FdEvents ready) -> void { handle_fd_event(ready); });
}

} // namespace jb::net
