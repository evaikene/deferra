#include "local_server.hpp"

#include "local_ipc_macos_priv.hpp"
#include "local_server_priv.hpp"
#include "local_socket_priv.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace jb::net {

namespace {

constexpr int kInvalidFd{-1};

class UniqueFd {
public:
    explicit UniqueFd(int fd = kInvalidFd) noexcept
        : _fd{fd}
    {}

    ~UniqueFd()
    {
        if (_fd != kInvalidFd) {
            ::close(_fd);
        }
    }

    UniqueFd(UniqueFd const&)                    = delete;
    UniqueFd(UniqueFd&&)                         = delete;
    auto operator=(UniqueFd const&) -> UniqueFd& = delete;
    auto operator=(UniqueFd&&) -> UniqueFd&      = delete;

    [[nodiscard]] auto get() const noexcept -> int { return _fd; }

    [[nodiscard]] auto release() noexcept -> int { return std::exchange(_fd, kInvalidFd); }

private:
    int _fd;
};

auto system_error_message(std::string_view operation, int error) -> std::string
{
    return std::string{operation} + ": " + std::strerror(error);
}

auto is_resource_error(int error) -> bool
{
    return error == EMFILE || error == ENFILE || error == ENOMEM || error == ENOBUFS;
}

auto native_open_error(int error) -> jb::core::IOError
{
    return is_resource_error(error) ? jb::core::IOError::ResourceError : jb::core::IOError::OpenError;
}

void store_error(auto& data, jb::core::IOError error, std::string message)
{
    data.error        = error;
    data.error_string = std::move(message);
}

void clear_error(auto& data)
{
    data.error = jb::core::IOError::NoError;
    data.error_string.clear();
}

auto create_listener_socket() -> int
{
    int fd;
    for (;;) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 || errno != EINTR) {
            break;
        }
    }
    if (fd < 0) {
        return fd;
    }
    if (priv::configure_socket(fd)) {
        return fd;
    }

    auto const error = errno;
    ::close(fd);
    errno = error;
    return kInvalidFd;
}

auto inspect_path(std::filesystem::path const& path, struct stat& metadata) -> int
{
    for (;;) {
        auto const result = ::lstat(path.c_str(), &metadata);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

auto bind_listener(int fd, sockaddr_un const& address, socklen_t length) -> int
{
    for (;;) {
        auto const result = ::bind(fd, reinterpret_cast<sockaddr const*>(&address), length);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

auto apply_permissions(std::filesystem::path const& path, mode_t permissions) -> int
{
    for (;;) {
        auto const result = ::chmod(path.c_str(), permissions);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

auto start_listening(int fd, int backlog) -> int
{
    for (;;) {
        auto const result = ::listen(fd, backlog);
        if (result == 0 || errno != EINTR) {
            return result;
        }
    }
}

auto accept_connection(int fd) -> int
{
    for (;;) {
        auto const accepted_fd = ::accept(fd, nullptr, nullptr);
        if (accepted_fd >= 0 || errno != EINTR) {
            return accepted_fd;
        }
    }
}

auto query_peer_credentials(int fd, uid_t& user_id, gid_t& group_id) -> bool
{
    for (;;) {
        if (::getpeereid(fd, &user_id, &group_id) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

auto permissions_are_valid(std::filesystem::perms permissions) -> bool
{
    using PermissionBits = std::underlying_type_t<std::filesystem::perms>;

    if (permissions == std::filesystem::perms::unknown) {
        return false;
    }

    auto const bits = static_cast<PermissionBits>(permissions);
    auto const mask = static_cast<PermissionBits>(std::filesystem::perms::mask);
    return (bits & static_cast<PermissionBits>(~mask)) == 0;
}

auto metadata_matches(auto const& data, struct stat const& metadata) -> bool
{
    return S_ISSOCK(metadata.st_mode) && static_cast<std::uintmax_t>(metadata.st_dev) == data.path_device &&
           static_cast<std::uintmax_t>(metadata.st_ino) == data.path_inode;
}

auto cleanup_owned_path(auto& data) -> bool
{
    if (!data.owns_path) {
        return true;
    }

    struct stat metadata{};
    if (inspect_path(data.server_path, metadata) < 0) {
        auto const error = errno;
        if (error == ENOENT) {
            data.owns_path = false;
            return true;
        }
        store_error(data,
                    jb::core::IOError::CloseError,
                    system_error_message("local server socket path inspection during cleanup failed", error));
        return false;
    }

    if (!metadata_matches(data, metadata)) {
        data.owns_path = false;
        return true;
    }

    for (;;) {
        if (::unlink(data.server_path.c_str()) == 0) {
            data.owns_path = false;
            return true;
        }

        auto const error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error == ENOENT) {
            data.owns_path = false;
            return true;
        }
        store_error(data,
                    jb::core::IOError::CloseError,
                    system_error_message("local server socket path removal failed", error));
        return false;
    }
}

} // anonymous namespace

LocalServer::LocalServer(jb::core::Object* parent)
    : Object(*new Private, parent)
{}

LocalServer::~LocalServer()
{
    close();
}

auto LocalServer::listen(std::filesystem::path const& path, LocalServerOptions options) -> bool
{
    auto* d = d_ptr<Private>();

    if (d->listening) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server is already listening");
        return false;
    }

    if (d->owns_path && !cleanup_owned_path(*d)) {
        return false;
    }

    clear_error(*d);
    d->server_path = path;

    if (options.backlog <= 0) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server backlog must be positive");
        return false;
    }
    if (options.max_pending_connections == 0) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server pending connection limit must be positive");
        return false;
    }
    if (!permissions_are_valid(options.permissions)) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server permissions are invalid");
        return false;
    }

    auto const& native_path = path.native();
    if (native_path.empty()) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server path must not be empty");
        return false;
    }
    if (native_path.find('\0') != std::string::npos) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server path must not contain NUL bytes");
        return false;
    }

    sockaddr_un address{};
    if (native_path.size() + 1U > sizeof(address.sun_path)) {
        store_error(*d, jb::core::IOError::InvalidArgument, "local server path is too long");
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native_path.c_str(), native_path.size() + 1U);
    auto const address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native_path.size() + 1U);

    auto* loop = event_loop();
    if (!loop) {
        store_error(*d, jb::core::IOError::ResourceError, "local server requires an event loop");
        return false;
    }

    struct stat existing_metadata{};
    if (inspect_path(path, existing_metadata) == 0) {
        store_error(*d, jb::core::IOError::OpenError, "local server path already exists");
        return false;
    }
    if (errno != ENOENT) {
        auto const error = errno;
        store_error(*d,
                    jb::core::IOError::OpenError,
                    system_error_message("local server path inspection failed", error));
        return false;
    }

    UniqueFd listener{create_listener_socket()};
    if (listener.get() == kInvalidFd) {
        auto const error = errno;
        store_error(*d, native_open_error(error), system_error_message("local server socket creation failed", error));
        return false;
    }

    if (bind_listener(listener.get(), address, address_length) < 0) {
        auto const error = errno;
        store_error(*d, jb::core::IOError::OpenError, system_error_message("local server bind failed", error));
        return false;
    }

    struct stat bound_metadata{};
    if (inspect_path(path, bound_metadata) < 0) {
        auto const error = errno;
        store_error(*d,
                    jb::core::IOError::OpenError,
                    system_error_message("local server bound path inspection failed", error));
        return false;
    }

    d->path_device = static_cast<std::uintmax_t>(bound_metadata.st_dev);
    d->path_inode  = static_cast<std::uintmax_t>(bound_metadata.st_ino);
    d->owns_path   = true;

    auto fail_after_bind = [&](std::string_view operation, int error) -> bool {
        store_error(*d, jb::core::IOError::OpenError, system_error_message(operation, error));
        static_cast<void>(cleanup_owned_path(*d));
        return false;
    };

    if (!S_ISSOCK(bound_metadata.st_mode)) {
        store_error(*d, jb::core::IOError::OpenError, "local server bound path is not a socket");
        static_cast<void>(cleanup_owned_path(*d));
        return false;
    }

    using PermissionBits       = std::underlying_type_t<std::filesystem::perms>;
    auto const permission_bits = static_cast<PermissionBits>(options.permissions);
    if (apply_permissions(path, static_cast<mode_t>(permission_bits)) < 0) {
        return fail_after_bind("local server permission update failed", errno);
    }

    struct stat permission_metadata{};
    if (inspect_path(path, permission_metadata) < 0) {
        return fail_after_bind("local server path verification failed", errno);
    }
    if (!metadata_matches(*d, permission_metadata)) {
        store_error(*d, jb::core::IOError::OpenError, "local server socket path changed during setup");
        static_cast<void>(cleanup_owned_path(*d));
        return false;
    }

    if (start_listening(listener.get(), options.backlog) < 0) {
        return fail_after_bind("local server listen failed", errno);
    }

    d->options   = options;
    d->fd        = listener.release();
    d->listening = true;
    ++d->generation;
    auto const callback_generation = d->generation;

    d->accept_callback = [this, callback_generation](int ready_fd, jb::core::FdEvents events) -> void {
        auto*      owner             = this;
        auto const active_generation = callback_generation;
        auto*      data              = owner->d_ptr<Private>();
        if (!events.test(jb::core::FdEvent::Read) || !data->listening || data->fd != ready_fd ||
            data->generation != active_generation) {
            return;
        }

        auto const report_accept_error = [&](jb::core::IOError error, std::string message) -> void {
            store_error(*data, error, std::move(message));
            owner->emit(owner->accept_error, data->error, data->error_string);
        };
        auto const listener_is_current = [&]() -> bool {
            return data->listening && data->fd == ready_fd && data->generation == active_generation;
        };

        bool queued_connection = false;
        while (data->pending_connections.size() < data->options.max_pending_connections) {
            UniqueFd accepted{accept_connection(ready_fd)};
            if (accepted.get() == kInvalidFd) {
                auto const error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    break;
                }
                report_accept_error(native_open_error(error),
                                    system_error_message("local server accept failed", error));
                break;
            }

            if (!priv::configure_socket(accepted.get())) {
                auto const error = errno;
                report_accept_error(native_open_error(error),
                                    system_error_message("local server accepted socket configuration failed", error));
                if (!listener_is_current()) {
                    return;
                }
                continue;
            }

            uid_t user_id{};
            gid_t group_id{};
            if (!query_peer_credentials(accepted.get(), user_id, group_id)) {
                auto const error = errno;
                report_accept_error(native_open_error(error),
                                    system_error_message("local server peer credential query failed", error));
                if (!listener_is_current()) {
                    return;
                }
                continue;
            }

            auto private_data               = std::make_unique<LocalSocket::Private>();
            private_data->state             = LocalSocketState::Connected;
            private_data->server_path       = data->server_path;
            private_data->read_buffer_limit = data->options.accepted_read_buffer_limit;
            private_data->fd                = accepted.get();
            private_data->generation        = 1;
            private_data->peer_credentials.process_id.reset();
            private_data->peer_credentials.user_id  = static_cast<std::uint64_t>(user_id);
            private_data->peer_credentials.group_id = static_cast<std::uint64_t>(group_id);

            auto* socket_data = private_data.get();
            auto  socket      = std::unique_ptr<LocalSocket>{
                new LocalSocket{*private_data.release(), nullptr}
            };
            static_cast<void>(accepted.release());

            if (!socket_data->update_watch(*socket)) {
                report_accept_error(jb::core::IOError::ResourceError,
                                    "accepted local socket event-loop watch registration failed");
                if (!listener_is_current()) {
                    return;
                }
                continue;
            }
            data->pending_connections.push_back(std::move(socket));
            queued_connection = true;

            if (!listener_is_current()) {
                return;
            }
        }

        if (data->pending_connections.size() >= data->options.max_pending_connections && data->watch) {
            auto* loop = owner->event_loop();
            if (loop && loop->unwatch_fd(data->watch)) {
                data->watch = {};
            }
            else {
                report_accept_error(jb::core::IOError::ResourceError,
                                    "local server event-loop watch removal failed while pausing acceptance");
                if (!listener_is_current()) {
                    return;
                }
            }
        }

        if (queued_connection && listener_is_current()) {
            owner->emit(owner->new_connection);
        }
    };

    d->watch = loop->watch_fd(d->fd, jb::core::FdEvent::Read, d->accept_callback);
    if (!d->watch) {
        d->listening = false;
        ++d->generation;
        d->accept_callback = {};
        store_error(*d, jb::core::IOError::ResourceError, "local server event-loop watch registration failed");
        static_cast<void>(cleanup_owned_path(*d));
        auto const released_fd = std::exchange(d->fd, kInvalidFd);
        ::close(released_fd);
        return false;
    }

    return true;
}

void LocalServer::close()
{
    auto* d = d_ptr<Private>();
    if (!d->listening && d->fd == kInvalidFd && !d->owns_path && d->pending_connections.empty()) {
        return;
    }

    ++d->generation;
    auto*      loop          = event_loop();
    auto const watch         = d->watch;
    auto const retry_unwatch = loop && watch && !loop->unwatch_fd(watch);
    d->listening             = false;
    d->accept_callback       = {};
    d->pending_connections.clear();

    static_cast<void>(cleanup_owned_path(*d));

    auto const released_fd = std::exchange(d->fd, kInvalidFd);
    if (released_fd != kInvalidFd && ::close(released_fd) < 0) {
        auto const error = errno;
        store_error(*d,
                    jb::core::IOError::CloseError,
                    system_error_message("local server listener close failed", error));
    }
    if (retry_unwatch) {
        static_cast<void>(loop->unwatch_fd(watch));
    }
    d->watch = {};
}

auto LocalServer::is_listening() const noexcept -> bool
{
    return d_ptr<Private const>()->listening;
}

auto LocalServer::server_path() const noexcept -> std::filesystem::path const&
{
    return d_ptr<Private const>()->server_path;
}

auto LocalServer::pending_connection_count() const noexcept -> std::size_t
{
    return d_ptr<Private const>()->pending_connections.size();
}

auto LocalServer::take_next_connection() -> std::unique_ptr<LocalSocket>
{
    auto* d = d_ptr<Private>();
    if (d->pending_connections.empty()) {
        return {};
    }

    auto const was_paused =
        d->listening && !d->watch && d->pending_connections.size() >= d->options.max_pending_connections;
    auto socket = std::move(d->pending_connections.front());
    d->pending_connections.pop_front();

    if (was_paused) {
        auto* loop = event_loop();
        if (loop) {
            d->watch = loop->watch_fd(d->fd, jb::core::FdEvent::Read, d->accept_callback);
        }
        if (!loop || !d->watch) {
            store_error(*d, jb::core::IOError::ResourceError, "local server event-loop watch rearm failed");
            emit(accept_error, d->error, d->error_string);
        }
    }

    return socket;
}

auto LocalServer::error() const noexcept -> jb::core::IOError
{
    return d_ptr<Private const>()->error;
}

auto LocalServer::error_string() const noexcept -> std::string const&
{
    return d_ptr<Private const>()->error_string;
}

} // namespace jb::net
