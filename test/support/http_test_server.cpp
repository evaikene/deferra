#include "http_test_server.hpp"

#include <array>
#include <cerrno>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jb::test {

namespace {

constexpr std::string_view kResponse{"HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nstage53-body"};

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags{MSG_NOSIGNAL};
#else
constexpr int kSendFlags{0};
#endif

void close_fd(int fd) noexcept
{
    if (fd >= 0) {
        static_cast<void>(::close(fd));
    }
}

auto socket_error(std::string_view operation) -> std::system_error
{
    return {errno, std::generic_category(), std::string{operation}};
}

auto send_all(int fd, std::string_view bytes) noexcept -> bool
{
    auto sent = std::size_t{0};
    while (sent < bytes.size()) {
        auto const count = ::send(fd, bytes.data() + sent, bytes.size() - sent, kSendFlags);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

} // anonymous namespace

HttpTestServer::HttpTestServer()
{
    _listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) {
        throw socket_error("socket");
    }

    auto reuse_address = 1;
    if (::setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        auto error = socket_error("setsockopt");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }

    auto address            = sockaddr_in{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(_listen_fd, 8) < 0) {
        auto error = socket_error("bind/listen");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }

    auto address_size = socklen_t{sizeof(address)};
    if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        auto error = socket_error("getsockname");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }
    _port = ntohs(address.sin_port);

    auto const listen_fd = _listen_fd;
    _accept_thread       = std::jthread{[this, listen_fd]() -> void { accept_connections(listen_fd); }};
}

HttpTestServer::~HttpTestServer()
{
    {
        std::lock_guard lock{_mutex};
        _stopping           = true;
        _responses_released = true;
    }
    _condition.notify_all();

    auto const listen_fd = std::exchange(_listen_fd, -1);
    if (listen_fd >= 0) {
        static_cast<void>(::shutdown(listen_fd, SHUT_RDWR));
        close_fd(listen_fd);
    }
    if (_accept_thread.joinable()) {
        _accept_thread.join();
    }
    for (auto& connection : _connection_threads) {
        if (connection.joinable()) {
            connection.join();
        }
    }
}

auto HttpTestServer::url(std::string path) const -> std::string
{
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return "http://127.0.0.1:" + std::to_string(_port) + path;
}

auto HttpTestServer::wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) -> bool
{
    std::unique_lock lock{_mutex};
    return _condition.wait_for(lock, timeout, [this, count]() -> bool {
        return _request_count >= count || _stopping;
    }) && _request_count >= count;
}

void HttpTestServer::release_responses()
{
    {
        std::lock_guard lock{_mutex};
        _responses_released = true;
    }
    _condition.notify_all();
}

void HttpTestServer::accept_connections(int listen_fd)
{
    for (;;) {
        auto const connection_fd = ::accept(listen_fd, nullptr, nullptr);
        if (connection_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        _connection_threads.emplace_back([this, connection_fd]() -> void { handle_connection(connection_fd); });
    }
}

void HttpTestServer::handle_connection(int connection_fd)
{
#if defined(SO_NOSIGPIPE)
    auto no_sigpipe = 1;
    static_cast<void>(::setsockopt(connection_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)));
#endif

    auto request = std::string{};
    for (;;) {
        auto       buffer = std::array<char, 1024>{};
        auto const count  = ::recv(connection_fd, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            request.append(buffer.data(), static_cast<std::size_t>(count));
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        close_fd(connection_fd);
        return;
    }

    {
        std::unique_lock lock{_mutex};
        ++_request_count;
        _condition.notify_all();
        _condition.wait(lock, [this]() -> bool { return _responses_released || _stopping; });
        if (_stopping) {
            close_fd(connection_fd);
            return;
        }
    }

    static_cast<void>(send_all(connection_fd, kResponse));
    static_cast<void>(::shutdown(connection_fd, SHUT_RDWR));
    close_fd(connection_fd);
}

} // namespace jb::test
