#include "tcp_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jb::net {

struct TcpSocket::Private {
    SocketState       state{SocketState::Unconnected};
    std::string       peer_address;
    std::uint16_t     peer_port{0};
    std::string       input_buffer;
    std::string       output_buffer;
    int               fd{-1};
    jb::core::FdWatch watch;
};

namespace {

constexpr int         kInvalidFd{-1};
constexpr std::size_t kBufferSize{16 * 1024};

auto system_error_message(std::string_view context) -> std::string
{
    std::string message{context};
    message += ": ";
    message += std::strerror(errno);
    return message;
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
    : IODevice(parent)
    , _d_socket(new Private)
{}

TcpSocket::~TcpSocket()
{
    close_socket(false);
    delete _d_socket;
}

void TcpSocket::connect_to_host(std::string_view address, std::uint16_t port)
{
    close_socket(false);
    _d_socket->input_buffer.clear();
    _d_socket->output_buffer.clear();
    clear_error();
    _d_socket->peer_address.assign(address);
    _d_socket->peer_port = port;

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
        set_error(jb::core::IOError::OpenError, system_error_message("socket failed"));
        return;
    }

    if (!disable_sigpipe(fd)) {
        auto message = system_error_message("setsockopt failed");
        ::close(fd);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    if (!set_nonblocking(fd)) {
        auto message = system_error_message("fcntl failed");
        ::close(fd);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    _d_socket->fd = fd;
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

    auto message = system_error_message("connect failed");
    close_socket(false);
    set_error(jb::core::IOError::OpenError, std::move(message));
}

void TcpSocket::disconnect_from_host()
{
    if (_d_socket->state == SocketState::Unconnected) {
        return;
    }

    if (_d_socket->output_buffer.empty()) {
        close_socket(true);
        return;
    }

    set_state(SocketState::Closing);
    update_watch();
    write_pending();
}

void TcpSocket::abort()
{
    auto const was_connected = _d_socket->state != SocketState::Unconnected;

    _d_socket->input_buffer.clear();
    _d_socket->output_buffer.clear();
    close_socket(was_connected);
}

auto TcpSocket::state() const noexcept -> SocketState
{
    return _d_socket->state;
}

auto TcpSocket::peer_address() const noexcept -> std::string const&
{
    return _d_socket->peer_address;
}

auto TcpSocket::peer_port() const noexcept -> std::uint16_t
{
    return _d_socket->peer_port;
}

auto TcpSocket::is_open() const -> bool
{
    return _d_socket->state != SocketState::Unconnected;
}

void TcpSocket::close()
{
    disconnect_from_host();
}

auto TcpSocket::read(std::size_t max_size) -> std::string
{
    if (_d_socket->input_buffer.empty()) {
        return {};
    }

    clear_error();
    auto const size = std::min(max_size, _d_socket->input_buffer.size());
    auto       out  = _d_socket->input_buffer.substr(0, size);
    _d_socket->input_buffer.erase(0, size);
    return out;
}

auto TcpSocket::read_all() -> std::string
{
    clear_error();
    auto out = std::move(_d_socket->input_buffer);
    _d_socket->input_buffer.clear();
    return out;
}

auto TcpSocket::read_line(std::size_t max_size) -> std::string
{
    auto const newline = _d_socket->input_buffer.find('\n');
    if (newline == std::string::npos && _d_socket->state != SocketState::Unconnected) {
        return {};
    }

    if (newline == std::string::npos && _d_socket->input_buffer.empty()) {
        return {};
    }

    clear_error();

    auto const has_newline = newline != std::string::npos && newline < max_size;
    auto const line_size   = has_newline ? newline + 1 : std::min(max_size, _d_socket->input_buffer.size());

    auto line = _d_socket->input_buffer.substr(0, line_size);
    _d_socket->input_buffer.erase(0, line_size);

    if (has_newline) {
        strip_crlf(line);
    }

    return line;
}

auto TcpSocket::can_read_line() const -> bool
{
    return _d_socket->input_buffer.find('\n') != std::string::npos ||
           (_d_socket->state == SocketState::Unconnected && !_d_socket->input_buffer.empty());
}

auto TcpSocket::write(std::string_view data) -> std::size_t
{
    if (_d_socket->state == SocketState::Unconnected) {
        set_error(jb::core::IOError::NotOpen, "socket is not connected");
        return 0;
    }

    clear_error();
    _d_socket->output_buffer.append(data);
    update_watch();
    if (_d_socket->state == SocketState::Connected || _d_socket->state == SocketState::Closing) {
        write_pending();
    }
    return data.size();
}

auto TcpSocket::bytes_available() const -> std::size_t
{
    return _d_socket->input_buffer.size();
}

void TcpSocket::set_state(SocketState state)
{
    _d_socket->state = state;
}

void TcpSocket::close_socket(bool emit_disconnected)
{
    auto* loop = event_loop();
    if (loop && _d_socket->watch) {
        loop->unwatch_fd(_d_socket->watch);
    }
    _d_socket->watch = {};

    if (_d_socket->fd != kInvalidFd) {
        ::close(_d_socket->fd);
    }
    _d_socket->fd = kInvalidFd;
    set_state(SocketState::Unconnected);

    if (emit_disconnected) {
        emit(disconnected);
    }
}

void TcpSocket::handle_fd_event(jb::core::FdEvents events)
{
    if (_d_socket->state == SocketState::Unconnected) {
        return;
    }

    if (_d_socket->state == SocketState::Connecting && events.test_any(jb::core::FdEvents{jb::core::FdEvent::Write})) {
        handle_connect_ready();
    }

    if (_d_socket->state == SocketState::Connected || _d_socket->state == SocketState::Closing) {
        if (events.test_any(jb::core::FdEvents{jb::core::FdEvent::Read})) {
            read_available();
        }
        if (_d_socket->state != SocketState::Unconnected &&
            events.test_any(jb::core::FdEvents{jb::core::FdEvent::Write})) {
            write_pending();
        }
    }

    update_watch();
}

void TcpSocket::handle_connect_ready()
{
    int       error = 0;
    socklen_t len   = sizeof(error);
    if (::getsockopt(_d_socket->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        auto message = system_error_message("getsockopt failed");
        close_socket(false);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    if (error != 0) {
        auto message = std::string{"connect failed: "} + std::strerror(error);
        close_socket(false);
        set_error(jb::core::IOError::OpenError, std::move(message));
        return;
    }

    set_state(SocketState::Connected);
    emit(connected);
    write_pending();
}

void TcpSocket::read_available()
{
    bool read_any = false;

    for (;;) {
        char       buffer[kBufferSize];
        auto const n = ::recv(_d_socket->fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            _d_socket->input_buffer.append(buffer, static_cast<std::size_t>(n));
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

        auto message = system_error_message("recv failed");
        close_socket(true);
        set_error(jb::core::IOError::ReadError, std::move(message));
        return;
    }

    if (read_any) {
        emit_ready_read();
    }
}

void TcpSocket::write_pending()
{
    while (!_d_socket->output_buffer.empty()) {
        auto const n =
            ::send(_d_socket->fd, _d_socket->output_buffer.data(), _d_socket->output_buffer.size(), send_flags());
        if (n > 0) {
            auto const size = static_cast<std::size_t>(n);
            _d_socket->output_buffer.erase(0, size);
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

        auto message = system_error_message("send failed");
        close_socket(true);
        set_error(jb::core::IOError::WriteError, std::move(message));
        return;
    }

    if (_d_socket->state == SocketState::Closing && _d_socket->output_buffer.empty()) {
        close_socket(true);
    }
}

void TcpSocket::update_watch()
{
    auto* loop = event_loop();
    if (!loop || _d_socket->fd == kInvalidFd) {
        return;
    }

    jb::core::FdEvents events;
    if (_d_socket->state == SocketState::Connecting) {
        events.set(jb::core::FdEvent::Write);
    }
    if (_d_socket->state == SocketState::Connected || _d_socket->state == SocketState::Closing) {
        events.set(jb::core::FdEvent::Read);
        if (!_d_socket->output_buffer.empty() || _d_socket->state == SocketState::Closing) {
            events.set(jb::core::FdEvent::Write);
        }
    }

    if (events.none()) {
        if (_d_socket->watch) {
            loop->unwatch_fd(_d_socket->watch);
            _d_socket->watch = {};
        }
        return;
    }

    _d_socket->watch = loop->watch_fd(_d_socket->fd, events, [this](int, jb::core::FdEvents ready) -> void {
        handle_fd_event(ready);
    });
}

} // namespace jb::net
