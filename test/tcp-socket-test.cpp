#include "application.hpp"
#include "tcp_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::net;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

class TestServer {
public:

    TestServer()
    {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(_fd >= 0);

        int yes = 1;
        REQUIRE(::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        REQUIRE(::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(_fd, 1) == 0);

        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        _port = ntohs(addr.sin_port);
        set_nonblocking(_fd);
    }

    ~TestServer()
    {
        if (_accepted_fd >= 0) {
            ::close(_accepted_fd);
        }
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    TestServer(TestServer const&)                    = delete;
    TestServer(TestServer&&)                         = delete;
    auto operator=(TestServer const&) -> TestServer& = delete;
    auto operator=(TestServer&&) -> TestServer&      = delete;

    auto port() const -> std::uint16_t { return _port; }

    auto accept_client() -> bool
    {
        if (_accepted_fd >= 0) {
            return true;
        }

        _accepted_fd = ::accept(_fd, nullptr, nullptr);
        if (_accepted_fd < 0) {
            return false;
        }

        set_nonblocking(_accepted_fd);
        return true;
    }

    void write_to_client(std::string_view data) const
    {
        REQUIRE(_accepted_fd >= 0);

        std::size_t sent = 0;
        while (sent < data.size()) {
            auto const n = ::send(_accepted_fd, data.data() + sent, data.size() - sent, 0);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }
            FAIL("send failed: " << std::strerror(errno));
        }
    }

    auto read_from_client() -> std::string
    {
        REQUIRE(_accepted_fd >= 0);

        for (;;) {
            std::array<char, 1024> buffer{};
            auto const             n = ::recv(_accepted_fd, buffer.data(), buffer.size(), 0);
            if (n > 0) {
                _client_read_buffer.append(buffer.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }
        return _client_read_buffer;
    }

    void close_client()
    {
        REQUIRE(_accepted_fd >= 0);
        ::close(_accepted_fd);
        _accepted_fd = -1;
    }

    void reset_client()
    {
        REQUIRE(_accepted_fd >= 0);

        linger reset{1, 0};
        REQUIRE(::setsockopt(_accepted_fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
        ::close(_accepted_fd);
        _accepted_fd = -1;
    }

private:
    static void set_nonblocking(int fd)
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        REQUIRE(flags >= 0);
        REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    }

    int           _fd{-1};
    int           _accepted_fd{-1};
    std::uint16_t _port{0};
    std::string   _client_read_buffer;
};

template <typename Predicate>
auto wait_for(Application& app, Predicate&& pred) -> bool
{
    for (int i = 0; i < 20; ++i) {
        if (pred()) {
            return true;
        }
        app.process_events(EventFlag::All, 50);
    }
    return pred();
}

auto connect_socket(Application& app, TestServer& server, TcpSocket& socket) -> bool
{
    socket.connect_to_host("127.0.0.1", server.port());
    return wait_for(app, [&]() -> bool { return server.accept_client() && socket.state() == SocketState::Connected; });
}

} // anonymous namespace

TEST_CASE("TcpSocket starts unconnected", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TcpSocket   socket;

    CHECK(socket.state() == SocketState::Unconnected);
    CHECK_FALSE(socket.is_open());
    CHECK(socket.bytes_available() == 0);
    CHECK(socket.peer_address().empty());
    CHECK(socket.peer_port() == 0);
    CHECK(socket.error() == jb::core::IOError::NoError);
}

TEST_CASE("TcpSocket connects to a loopback server", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;
    int         connected_count = 0;

    socket.connected.connect([&]() -> void { ++connected_count; });

    REQUIRE(connect_socket(app, server, socket));

    CHECK(socket.state() == SocketState::Connected);
    CHECK(socket.is_open());
    CHECK(socket.peer_address() == "127.0.0.1");
    CHECK(socket.peer_port() == server.port());
    CHECK(socket.error() == jb::core::IOError::NoError);
    CHECK(connected_count == 1);
}

TEST_CASE("TcpSocket clears buffered data when reconnecting", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  first_server;
    TcpSocket   socket;
    int         closed_count = 0;

    socket.closed.connect([&]() -> void { ++closed_count; });

    REQUIRE(connect_socket(app, first_server, socket));

    first_server.write_to_client("stale\n");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.can_read_line(); }));
    CHECK(socket.bytes_available() == 6);

    TestServer second_server;
    socket.connect_to_host("127.0.0.1", second_server.port());
    CHECK(closed_count == 1);
    REQUIRE(wait_for(app, [&]() -> bool {
        return second_server.accept_client() && socket.state() == SocketState::Connected;
    }));

    CHECK(socket.bytes_available() == 0);
    CHECK_FALSE(socket.can_read_line());

    socket.abort();
    CHECK(closed_count == 2);
}

TEST_CASE("TcpSocket read API should return empty buffered data", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TcpSocket   socket;

    CHECK(socket.read(10).empty());
    CHECK(socket.read_all().empty());
    CHECK(socket.bytes_available() == 0);
}

TEST_CASE("TcpSocket write reports NotOpen before connection", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TcpSocket   socket;

    CHECK(socket.write("abc") == 0);
    CHECK(socket.error() == jb::core::IOError::NotOpen);
    CHECK_FALSE(socket.error_string().empty());
}

TEST_CASE("TcpSocket close and abort are no-ops while unconnected", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TcpSocket   socket;
    int         disconnected_count = 0;
    int         closed_count       = 0;

    socket.disconnected.connect([&]() -> void { ++disconnected_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });

    socket.close();
    socket.abort();

    CHECK(socket.state() == SocketState::Unconnected);
    CHECK(disconnected_count == 0);
    CHECK(closed_count == 0);
}

TEST_CASE("TcpSocket receives data and emits readyRead", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;
    int         ready_count = 0;

    socket.ready_read.connect([&]() -> void { ++ready_count; });
    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("hello");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 5; }));

    CHECK(ready_count == 1);
    CHECK(socket.read(2) == "he");
    CHECK(socket.bytes_available() == 3);
    CHECK(socket.read_all() == "llo");
    CHECK(socket.bytes_available() == 0);
}

TEST_CASE("TcpSocket reads complete buffered text lines", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;

    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("first\r\nsecond");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 13; }));

    CHECK(socket.can_read_line());
    CHECK(socket.read_line() == "first");
    CHECK_FALSE(socket.can_read_line());
    CHECK(socket.read_line().empty());

    server.write_to_client("\n");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.can_read_line(); }));
    CHECK(socket.read_line() == "second");
}

TEST_CASE("TcpSocket distinguishes empty buffered lines from no line", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;

    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("\n");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.can_read_line(); }));

    CHECK(socket.can_read_line());
    CHECK(socket.read_line().empty());
    CHECK_FALSE(socket.can_read_line());
}

TEST_CASE("TcpSocket writes buffered data and emits bytesWritten", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;
    std::size_t written = 0;

    socket.bytes_written.connect([&](std::size_t bytes) -> void { written += bytes; });
    REQUIRE(connect_socket(app, server, socket));

    CHECK(socket.write("ping") == 4);
    REQUIRE(wait_for(app, [&]() -> bool { return server.read_from_client() == "ping"; }));

    CHECK(written == 4);
}

TEST_CASE("TcpSocket preserves buffered bytes across graceful peer close", "[net][tcp-socket]")
{
    Application                   app{0, nullptr};
    TestServer                    server;
    TcpSocket                     socket;
    int                           disconnected_count = 0;
    int                           closed_count       = 0;
    std::vector<std::string_view> signal_order;

    socket.ready_read.connect([&]() -> void { signal_order.emplace_back("ready_read"); });
    socket.disconnected.connect([&]() -> void {
        ++disconnected_count;
        signal_order.emplace_back("disconnected");
    });
    socket.closed.connect([&]() -> void {
        ++closed_count;
        signal_order.emplace_back("closed");
    });
    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("bye");
    server.close_client();

    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == SocketState::Unconnected; }));
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
    auto const expected_order = std::vector<std::string_view>{"ready_read", "disconnected", "closed"};
    CHECK(signal_order == expected_order);
    CHECK(socket.can_read_line());
    CHECK(socket.read_line() == "bye");
    CHECK_FALSE(socket.can_read_line());
    CHECK(socket.read_all().empty());
}

TEST_CASE("TcpSocket read_line respects max_size", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;

    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("abcdef\n");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.can_read_line(); }));

    CHECK(socket.read_line(3) == "abc");
    CHECK(socket.read_line() == "def");
}

TEST_CASE("TcpSocket preserves read_all for buffered bytes after graceful peer close", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;

    REQUIRE(connect_socket(app, server, socket));

    server.write_to_client("bye");
    server.close_client();

    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == SocketState::Unconnected; }));
    CHECK(socket.read_all() == "bye");
}

TEST_CASE("TcpSocket emits closed once after a peer reset", "[net][tcp-socket]")
{
    Application                   app{0, nullptr};
    TestServer                    server;
    TcpSocket                     socket;
    int                           disconnected_count = 0;
    int                           closed_count       = 0;
    int                           error_count        = 0;
    std::vector<std::string_view> signal_order;

    socket.error_occurred.connect([&](IOError, std::string const&) -> void {
        ++error_count;
        signal_order.emplace_back("error_occurred");
    });
    socket.disconnected.connect([&]() -> void {
        ++disconnected_count;
        signal_order.emplace_back("disconnected");
    });
    socket.closed.connect([&]() -> void {
        ++closed_count;
        signal_order.emplace_back("closed");
    });
    REQUIRE(connect_socket(app, server, socket));

    server.reset_client();

    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == SocketState::Unconnected; }));
    CHECK(socket.error() == IOError::ReadError);
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
    CHECK(error_count == 1);
    auto const expected_order = std::vector<std::string_view>{"disconnected", "error_occurred", "closed"};
    CHECK(signal_order == expected_order);

    socket.close();
    socket.abort();
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
}

TEST_CASE("TcpSocket disconnect_from_host closes after queued writes flush", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    TcpSocket   socket;
    int         disconnected_count = 0;
    int         closed_count       = 0;

    socket.disconnected.connect([&]() -> void { ++disconnected_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });
    REQUIRE(connect_socket(app, server, socket));

    CHECK(socket.write("done") == 4);
    socket.disconnect_from_host();

    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == SocketState::Unconnected; }));
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
    REQUIRE(wait_for(app, [&]() -> bool { return server.read_from_client() == "done"; }));

    socket.close();
    socket.abort();
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
}

TEST_CASE("TcpSocket destruction does not emit closed", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TestServer  server;
    int         closed_count = 0;

    {
        TcpSocket socket;
        socket.closed.connect([&]() -> void { ++closed_count; });
        REQUIRE(connect_socket(app, server, socket));
    }

    CHECK(closed_count == 0);
}

TEST_CASE("TcpSocket rejects non-numeric addresses", "[net][tcp-socket]")
{
    Application app{0, nullptr};
    TcpSocket   socket;

    socket.connect_to_host("localhost", 80);

    CHECK(socket.state() == SocketState::Unconnected);
    CHECK(socket.error() == jb::core::IOError::InvalidArgument);
    CHECK_FALSE(socket.error_string().empty());
}

// NOLINTEND(readability-magic-numbers)
