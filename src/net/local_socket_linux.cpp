#include "local_socket.hpp"

#include "local_socket_priv.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace jb::net {

namespace {

constexpr int         kInvalidFd{-1};
constexpr std::size_t kBufferSize{static_cast<std::size_t>(16U * 1024U)};

auto system_error_message(std::string_view operation, int error) -> std::string
{
    return std::string{operation} + ": " + std::strerror(error);
}

auto is_resource_error(int error) -> bool
{
    return error == EMFILE || error == ENFILE || error == ENOMEM || error == ENOBUFS;
}

auto open_error(int error) -> jb::core::IOError
{
    return is_resource_error(error) ? jb::core::IOError::ResourceError : jb::core::IOError::OpenError;
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

auto create_socket() -> int
{
    for (;;) {
        auto const fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd >= 0 || errno != EINTR) {
            return fd;
        }
    }
}

auto socket_error(int fd, int& error) -> bool
{
    for (;;) {
        socklen_t length = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) {
            if (length == sizeof(error)) {
                return true;
            }
            errno = EIO;
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

auto query_peer_credentials(int fd, ucred& credentials) -> bool
{
    for (;;) {
        socklen_t length = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
            if (length == sizeof(credentials)) {
                return true;
            }
            errno = EIO;
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

} // anonymous namespace

LocalSocket::LocalSocket(jb::core::Object* parent)
    : IODevice(*new Private, parent)
{}

LocalSocket::LocalSocket(Private& dd, jb::core::Object* parent)
    : IODevice(dd, parent)
{}

LocalSocket::~LocalSocket()
{
    auto* d = d_ptr<Private>();
    static_cast<void>(d->release_socket(*this));
    d->input_buffer.clear();
    d->output_buffer.clear();
}

void LocalSocket::connect_to_server(std::filesystem::path const& path)
{
    auto* d = d_ptr<Private>();

    if (d->state != LocalSocketState::Unconnected) {
        abort();
    }

    d->input_buffer.clear();
    d->output_buffer.clear();
    d->server_path.clear();
    d->peer_credentials = {};
    clear_error();
    d->server_path = path;

    auto* loop = event_loop();
    if (!loop) {
        set_error(jb::core::IOError::ResourceError, "local socket requires an event loop");
        return;
    }

    auto const& native_path = path.native();
    if (native_path.empty()) {
        set_error(jb::core::IOError::InvalidArgument, "local socket path must not be empty");
        return;
    }
    if (native_path.find('\0') != std::string::npos) {
        set_error(jb::core::IOError::InvalidArgument, "local socket path must not contain NUL bytes");
        return;
    }

    sockaddr_un address{};
    if (native_path.size() + 1U > sizeof(address.sun_path)) {
        set_error(jb::core::IOError::InvalidArgument, "local socket path is too long");
        return;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native_path.c_str(), native_path.size() + 1U);
    auto const address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native_path.size() + 1U);

    auto const fd = create_socket();
    if (fd < 0) {
        auto const error = errno;
        set_error(open_error(error), system_error_message("local socket creation failed", error));
        return;
    }

    d->fd    = fd;
    d->state = LocalSocketState::Connecting;
    ++d->generation;

    auto const rc = ::connect(fd, reinterpret_cast<sockaddr const*>(&address), address_length);
    if (rc == 0) {
        d->complete_connection(*this);
        return;
    }

    auto const error = errno;
    if (error == EINPROGRESS || error == EALREADY || error == EINTR) {
        if (!d->update_watch(*this)) {
            d->fail_lifecycle(*this,
                              jb::core::IOError::ResourceError,
                              "local socket event-loop watch registration failed",
                              false);
        }
        return;
    }

    d->fail_lifecycle(*this, open_error(error), system_error_message("local socket connection failed", error), false);
}

void LocalSocket::disconnect_from_server()
{
    auto* d = d_ptr<Private>();

    if (d->state == LocalSocketState::Unconnected) {
        return;
    }
    if (d->state == LocalSocketState::Connecting) {
        d->output_buffer.clear();
        d->close_lifecycle(*this, true);
        return;
    }
    if (d->output_buffer.empty()) {
        d->close_lifecycle(*this, true);
        return;
    }

    d->state = LocalSocketState::Closing;
    if (!d->update_watch(*this)) {
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          true);
        return;
    }
    d->write_pending(*this);
}

void LocalSocket::abort()
{
    auto* d = d_ptr<Private>();

    if (d->state == LocalSocketState::Unconnected) {
        return;
    }

    d->input_buffer.clear();
    d->output_buffer.clear();
    d->close_lifecycle(*this, true);
}

auto LocalSocket::state() const noexcept -> LocalSocketState
{
    return d_ptr<Private const>()->state;
}

auto LocalSocket::server_path() const noexcept -> std::filesystem::path const&
{
    return d_ptr<Private const>()->server_path;
}

auto LocalSocket::peer_credentials() const noexcept -> LocalPeerCredentials const&
{
    return d_ptr<Private const>()->peer_credentials;
}

void LocalSocket::set_read_buffer_limit(std::size_t bytes)
{
    auto* d              = d_ptr<Private>();
    d->read_buffer_limit = bytes;
    if (!d->update_watch(*this)) {
        auto const emit_disconnected = d->state != LocalSocketState::Connecting;
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          emit_disconnected);
    }
}

auto LocalSocket::read_buffer_limit() const noexcept -> std::size_t
{
    return d_ptr<Private const>()->read_buffer_limit;
}

auto LocalSocket::is_open() const -> bool
{
    return d_ptr<Private const>()->state != LocalSocketState::Unconnected;
}

void LocalSocket::close()
{
    disconnect_from_server();
}

auto LocalSocket::read(std::size_t max_size) -> std::string
{
    auto* d = d_ptr<Private>();
    if (d->input_buffer.empty()) {
        return {};
    }

    clear_error();
    auto const size = std::min(max_size, d->input_buffer.size());
    auto       out  = d->input_buffer.substr(0, size);
    d->input_buffer.erase(0, size);
    if (!d->update_watch(*this)) {
        auto const emit_disconnected = d->state != LocalSocketState::Connecting;
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          emit_disconnected);
    }
    return out;
}

auto LocalSocket::read_all() -> std::string
{
    clear_error();

    auto* d   = d_ptr<Private>();
    auto  out = std::move(d->input_buffer);
    d->input_buffer.clear();
    if (!d->update_watch(*this)) {
        auto const emit_disconnected = d->state != LocalSocketState::Connecting;
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          emit_disconnected);
    }
    return out;
}

auto LocalSocket::read_line(std::size_t max_size) -> std::string
{
    auto*      d                 = d_ptr<Private>();
    auto const newline           = d->input_buffer.find('\n');
    auto const has_complete_line = newline != std::string::npos;
    auto const has_prefix        = max_size != static_cast<std::size_t>(-1) && max_size <= d->input_buffer.size();
    auto const has_final_line    = d->state == LocalSocketState::Unconnected && !d->input_buffer.empty();

    if (!has_complete_line && !has_prefix && !has_final_line) {
        return {};
    }

    clear_error();

    auto const includes_newline = has_complete_line && newline < max_size;
    auto const line_size        = includes_newline ? newline + 1U : std::min(max_size, d->input_buffer.size());

    auto line = d->input_buffer.substr(0, line_size);
    d->input_buffer.erase(0, line_size);
    if (includes_newline) {
        strip_crlf(line);
    }

    if (!d->update_watch(*this)) {
        auto const emit_disconnected = d->state != LocalSocketState::Connecting;
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          emit_disconnected);
    }
    return line;
}

auto LocalSocket::can_read_line() const -> bool
{
    auto const* d = d_ptr<Private const>();
    return d->input_buffer.find('\n') != std::string::npos ||
           (d->state == LocalSocketState::Unconnected && !d->input_buffer.empty());
}

auto LocalSocket::write(std::string_view data) -> std::size_t
{
    if (data.empty()) {
        return 0;
    }

    auto* d = d_ptr<Private>();
    if (d->state == LocalSocketState::Unconnected) {
        set_error(jb::core::IOError::NotOpen, "local socket is not connected");
        return 0;
    }

    clear_error();
    d->output_buffer.append(data);
    if (!d->update_watch(*this)) {
        auto const emit_disconnected = d->state != LocalSocketState::Connecting;
        d->fail_lifecycle(*this,
                          jb::core::IOError::ResourceError,
                          "local socket event-loop watch registration failed",
                          emit_disconnected);
        return 0;
    }
    if (d->state == LocalSocketState::Connected || d->state == LocalSocketState::Closing) {
        d->write_pending(*this);
    }
    return data.size();
}

auto LocalSocket::bytes_available() const -> std::size_t
{
    return d_ptr<Private const>()->input_buffer.size();
}

auto LocalSocket::Private::release_socket(LocalSocket& socket) -> bool
{
    auto const was_open = state != LocalSocketState::Unconnected;

    auto*      loop          = socket.event_loop();
    auto const active_watch  = watch;
    auto const retry_unwatch = loop != nullptr && active_watch && !loop->unwatch_fd(active_watch);

    auto const released_fd = std::exchange(fd, kInvalidFd);
    state                  = LocalSocketState::Unconnected;
    ++generation;
    if (released_fd != kInvalidFd) {
        ::close(released_fd);
    }
    if (retry_unwatch) {
        static_cast<void>(loop->unwatch_fd(active_watch));
    }
    watch = {};

    return was_open;
}

void LocalSocket::Private::close_lifecycle(LocalSocket& socket, bool emit_disconnected)
{
    auto const was_open = release_socket(socket);
    output_buffer.clear();

    if (!was_open) {
        return;
    }
    if (emit_disconnected) {
        socket.emit(socket.disconnected);
    }
    socket.emit_closed();
}

void LocalSocket::Private::fail_lifecycle(LocalSocket&      socket,
                                          jb::core::IOError error,
                                          std::string       message,
                                          bool              emit_disconnected)
{
    auto const was_open = release_socket(socket);
    output_buffer.clear();

    if (was_open && emit_disconnected) {
        socket.emit(socket.disconnected);
    }
    socket.set_error(error, std::move(message));
    if (was_open) {
        socket.emit_closed();
    }
}

void LocalSocket::Private::handle_fd_event(LocalSocket&       socket,
                                           int                ready_fd,
                                           std::uint64_t      callback_generation,
                                           jb::core::FdEvents events)
{
    if (state == LocalSocketState::Unconnected || fd != ready_fd || generation != callback_generation) {
        return;
    }

    auto const active_generation = generation;
    if (state == LocalSocketState::Connecting && events.any()) {
        complete_connection(socket);
    }
    if (generation != active_generation || fd != ready_fd) {
        return;
    }

    if ((state == LocalSocketState::Connected || state == LocalSocketState::Closing) &&
        events.test_any(jb::core::FdEvents{jb::core::FdEvent::Read})) {
        read_available(socket);
    }
    if (generation != active_generation || fd != ready_fd) {
        return;
    }

    if ((state == LocalSocketState::Connected || state == LocalSocketState::Closing) &&
        events.test_any(jb::core::FdEvents{jb::core::FdEvent::Write})) {
        write_pending(socket);
    }
    if (generation == active_generation && fd == ready_fd && !update_watch(socket)) {
        auto const emit_disconnected = state != LocalSocketState::Connecting;
        fail_lifecycle(socket,
                       jb::core::IOError::ResourceError,
                       "local socket event-loop watch registration failed",
                       emit_disconnected);
    }
}

void LocalSocket::Private::complete_connection(LocalSocket& socket)
{
    auto const active_generation = generation;
    auto const active_fd         = fd;

    int error = 0;
    if (!socket_error(active_fd, error)) {
        auto const native_error = errno;
        fail_lifecycle(socket,
                       open_error(native_error),
                       system_error_message("local socket connection query failed", native_error),
                       false);
        return;
    }
    if (error != 0) {
        fail_lifecycle(socket, open_error(error), system_error_message("local socket connection failed", error), false);
        return;
    }

    ucred credentials{};
    if (!query_peer_credentials(active_fd, credentials)) {
        auto const native_error = errno;
        fail_lifecycle(socket,
                       open_error(native_error),
                       system_error_message("local socket peer credential query failed", native_error),
                       false);
        return;
    }

    state = LocalSocketState::Connected;
    if (!update_watch(socket)) {
        fail_lifecycle(socket,
                       jb::core::IOError::ResourceError,
                       "local socket event-loop watch registration failed",
                       false);
        return;
    }

    peer_credentials.process_id = static_cast<std::uint64_t>(credentials.pid);
    peer_credentials.user_id    = static_cast<std::uint64_t>(credentials.uid);
    peer_credentials.group_id   = static_cast<std::uint64_t>(credentials.gid);
    socket.emit(socket.connected);

    if (generation == active_generation && fd == active_fd && state == LocalSocketState::Connected) {
        write_pending(socket);
    }
}

void LocalSocket::Private::read_available(LocalSocket& socket)
{
    bool read_any = false;

    for (;;) {
        auto const limited = read_buffer_limit != 0;
        if (limited && input_buffer.size() >= read_buffer_limit) {
            break;
        }

        auto const capacity = limited ? std::min(kBufferSize, read_buffer_limit - input_buffer.size()) : kBufferSize;
        char       buffer[kBufferSize];
        auto const n = ::recv(fd, buffer, capacity, 0);
        if (n > 0) {
            input_buffer.append(buffer, static_cast<std::size_t>(n));
            read_any = true;
            continue;
        }

        if (n == 0) {
            auto const was_open = release_socket(socket);
            output_buffer.clear();
            if (read_any) {
                socket.emit_ready_read();
            }
            if (was_open) {
                socket.emit(socket.disconnected);
                socket.emit_closed();
            }
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        auto const native_error = errno;
        auto const was_open     = release_socket(socket);
        output_buffer.clear();
        if (read_any) {
            socket.emit_ready_read();
        }
        if (was_open) {
            socket.emit(socket.disconnected);
        }
        socket.set_error(jb::core::IOError::ReadError,
                         system_error_message("local socket receive failed", native_error));
        if (was_open) {
            socket.emit_closed();
        }
        return;
    }

    if (!read_any) {
        return;
    }

    auto const read_paused = read_buffer_limit != 0 && input_buffer.size() >= read_buffer_limit;
    if (read_paused && !update_watch(socket)) {
        socket.emit_ready_read();
        fail_lifecycle(socket,
                       jb::core::IOError::ResourceError,
                       "local socket event-loop watch registration failed",
                       true);
        return;
    }
    socket.emit_ready_read();
}

void LocalSocket::Private::write_pending(LocalSocket& socket)
{
    auto const active_generation = generation;
    auto const active_fd         = fd;

    while (!output_buffer.empty()) {
        auto const n = ::send(active_fd, output_buffer.data(), output_buffer.size(), MSG_NOSIGNAL);
        if (n > 0) {
            auto const size = static_cast<std::size_t>(n);
            output_buffer.erase(0, size);
            socket.emit_bytes_written(size);
            if (generation != active_generation || fd != active_fd || state == LocalSocketState::Unconnected) {
                return;
            }
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

        auto const native_error    = errno;
        auto const was_established = state != LocalSocketState::Connecting;
        fail_lifecycle(socket,
                       jb::core::IOError::WriteError,
                       system_error_message("local socket send failed", native_error),
                       was_established);
        return;
    }

    if (state == LocalSocketState::Closing && output_buffer.empty()) {
        close_lifecycle(socket, true);
    }
}

auto LocalSocket::Private::update_watch(LocalSocket& socket) -> bool
{
    auto* loop = socket.event_loop();
    if (fd == kInvalidFd) {
        return true;
    }
    if (!loop) {
        return false;
    }

    jb::core::FdEvents events;
    if (state == LocalSocketState::Connecting) {
        events.set(jb::core::FdEvent::Read);
        events.set(jb::core::FdEvent::Write);
    }
    if (state == LocalSocketState::Connected || state == LocalSocketState::Closing) {
        if (read_buffer_limit == 0 || input_buffer.size() < read_buffer_limit) {
            events.set(jb::core::FdEvent::Read);
        }
        if (!output_buffer.empty()) {
            events.set(jb::core::FdEvent::Write);
        }
    }

    if (events.none()) {
        if (watch) {
            if (!loop->unwatch_fd(watch)) {
                return false;
            }
            watch = {};
        }
        return true;
    }

    auto*      owner               = &socket;
    auto const callback_generation = generation;
    auto const new_watch = loop->watch_fd(fd,
                                          events,
                                          jb::core::FdTriggerMode::Edge,
                                          [owner, callback_generation](int ready_fd, jb::core::FdEvents ready) -> void {
                                              auto* data = owner->d_ptr<Private>();
                                              data->handle_fd_event(*owner, ready_fd, callback_generation, ready);
                                          });
    if (!new_watch) {
        return false;
    }
    watch = new_watch;
    return true;
}

} // namespace jb::net
