#include "application.hpp"
#include "local_socket.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::net;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

constexpr int kInvalidFd{-1};

void close_fd(int& fd) noexcept
{
    if (fd >= 0) {
        ::close(fd);
        fd = kInvalidFd;
    }
}

[[noreturn]] void throw_system_error(std::string_view operation, int error)
{
    throw std::system_error{error, std::generic_category(), std::string{operation}};
}

class NativeLocalListener {
public:
    explicit NativeLocalListener(std::filesystem::path path)
        : _path{std::move(path)}
    {
        auto const& native_path = _path.native();
        if (native_path.empty() || native_path.find('\0') != std::string::npos) {
            throw std::invalid_argument{"invalid native listener path"};
        }

        sockaddr_un address{};
        if (native_path.size() + 1U > sizeof(address.sun_path)) {
            throw std::invalid_argument{"native listener path is too long"};
        }
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, native_path.c_str(), native_path.size() + 1U);
        auto const length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native_path.size() + 1U);

        _listener_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (_listener_fd < 0) {
            throw_system_error("native listener socket", errno);
        }

        if (::bind(_listener_fd, reinterpret_cast<sockaddr const*>(&address), length) < 0) {
            auto const error = errno;
            close_fd(_listener_fd);
            throw_system_error("native listener bind", error);
        }
        if (::listen(_listener_fd, 8) < 0) {
            auto const error = errno;
            close_fd(_listener_fd);
            std::error_code ignored;
            std::filesystem::remove(_path, ignored);
            throw_system_error("native listener listen", error);
        }
    }

    ~NativeLocalListener()
    {
        close_fd(_client_fd);
        close_fd(_listener_fd);
        std::error_code ignored;
        std::filesystem::remove(_path, ignored);
    }

    NativeLocalListener(NativeLocalListener const&)                    = delete;
    NativeLocalListener(NativeLocalListener&&)                         = delete;
    auto operator=(NativeLocalListener const&) -> NativeLocalListener& = delete;
    auto operator=(NativeLocalListener&&) -> NativeLocalListener&      = delete;

    [[nodiscard]] auto path() const noexcept -> std::filesystem::path const& { return _path; }

    auto accept_client() -> bool
    {
        if (_client_fd >= 0) {
            return true;
        }

        for (;;) {
            _client_fd = ::accept4(_listener_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (_client_fd >= 0) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            throw_system_error("native listener accept", errno);
        }
    }

    void queue_to_client(std::string_view data) { _pending_output.append(data); }

    auto flush_to_client() -> bool
    {
        if (_client_fd < 0) {
            return false;
        }

        while (!_pending_output.empty()) {
            auto const n = ::send(_client_fd, _pending_output.data(), _pending_output.size(), MSG_NOSIGNAL);
            if (n > 0) {
                _pending_output.erase(0, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            throw_system_error("native listener send", errno);
        }
        return true;
    }

    auto read_from_client() -> std::string const&
    {
        if (_client_fd < 0) {
            return _received_input;
        }

        for (;;) {
            char       buffer[16U * 1024U];
            auto const n = ::recv(_client_fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                _received_input.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                _peer_eof = true;
                return _received_input;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return _received_input;
            }
            throw_system_error("native listener receive", errno);
        }
    }

    void shutdown_writes()
    {
        REQUIRE(_client_fd >= 0);
        for (;;) {
            if (::shutdown(_client_fd, SHUT_WR) == 0) {
                return;
            }
            if (errno != EINTR) {
                throw_system_error("native listener write shutdown", errno);
            }
        }
    }

    void reset_client()
    {
        REQUIRE(_client_fd >= 0);
        static_cast<void>(::shutdown(_client_fd, SHUT_RDWR));
        close_fd(_client_fd);
    }

    [[nodiscard]] auto peer_eof() const noexcept -> bool { return _peer_eof; }

private:
    std::filesystem::path _path;
    int                   _listener_fd{kInvalidFd};
    int                   _client_fd{kInvalidFd};
    std::string           _pending_output;
    std::string           _received_input;
    bool                  _peer_eof{false};
};

template <typename Predicate>
auto wait_for(Application& app, Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::seconds{3})
    -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        app.process_events(EventFlag::All, 20);
    }
    return predicate();
}

auto connect_socket(Application& app, NativeLocalListener& listener, LocalSocket& socket) -> bool
{
    socket.connect_to_server(listener.path());
    return wait_for(app, [&]() -> bool {
        auto const accepted = listener.accept_client();
        return accepted && socket.state() == LocalSocketState::Connected;
    });
}

void send_to_socket(Application&         app,
                    NativeLocalListener& listener,
                    LocalSocket&         socket,
                    std::string_view     data,
                    std::size_t          expected_available)
{
    listener.queue_to_client(data);
    REQUIRE(wait_for(app, [&]() -> bool {
        auto const sent = listener.flush_to_client();
        return sent && socket.bytes_available() == expected_available;
    }));
}

} // anonymous namespace

TEST_CASE("LocalSocket exposes unconnected defaults and idempotent close", "[net][local-socket]")
{
    Application app{0, nullptr};
    LocalSocket socket;
    int         disconnected_count = 0;
    int         closed_count       = 0;
    int         error_count        = 0;

    socket.disconnected.connect([&]() -> void { ++disconnected_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });
    socket.error_occurred.connect([&](IOError, std::string const&) -> void { ++error_count; });

    CHECK(socket.state() == LocalSocketState::Unconnected);
    CHECK_FALSE(socket.is_open());
    CHECK(socket.server_path().empty());
    CHECK_FALSE(socket.peer_credentials().process_id);
    CHECK_FALSE(socket.peer_credentials().user_id);
    CHECK_FALSE(socket.peer_credentials().group_id);
    CHECK(socket.read_buffer_limit() == 0);
    CHECK(socket.bytes_available() == 0);
    CHECK(socket.error() == IOError::NoError);

    socket.close();
    socket.abort();
    CHECK(socket.write({}) == 0);
    CHECK(disconnected_count == 0);
    CHECK(closed_count == 0);
    CHECK(error_count == 0);

    CHECK(socket.write("data") == 0);
    CHECK(socket.error() == IOError::NotOpen);
    CHECK_FALSE(socket.error_string().empty());
    CHECK(error_count == 1);
}

TEST_CASE("LocalSocket requires an event loop", "[net][local-socket]")
{
    LocalSocket socket;
    int         error_count  = 0;
    int         closed_count = 0;

    socket.error_occurred.connect([&](IOError, std::string const&) -> void { ++error_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });
    socket.connect_to_server("unused.sock");

    CHECK(socket.state() == LocalSocketState::Unconnected);
    CHECK(socket.error() == IOError::ResourceError);
    CHECK_FALSE(socket.error_string().empty());
    CHECK(error_count == 1);
    CHECK(closed_count == 0);
}

TEST_CASE("LocalSocket rejects invalid filesystem paths safely", "[net][local-socket]")
{
    Application app{0, nullptr};
    LocalSocket socket;
    int         error_count  = 0;
    int         closed_count = 0;

    socket.error_occurred.connect([&](IOError, std::string const&) -> void { ++error_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });

    auto check_invalid = [&](std::filesystem::path const& path, std::string_view secret) -> void {
        socket.connect_to_server(path);
        CHECK(socket.state() == LocalSocketState::Unconnected);
        CHECK(socket.error() == IOError::InvalidArgument);
        CHECK_FALSE(socket.error_string().empty());
        CHECK(socket.error_string().find(secret) == std::string::npos);
    };

    check_invalid({}, "unused-secret");
    check_invalid(
        std::filesystem::path{
            std::string{"hidden\0path", 11}
    },
        "hidden");
    check_invalid(std::filesystem::path{std::string(256, 'x')}, "xxxx");

    CHECK(error_count == 3);
    CHECK(closed_count == 0);
}

TEST_CASE("LocalSocket reports connection failures and clears them on retry", "[net][local-socket]")
{
    Application                   app{0, nullptr};
    jb::test::TemporaryDirectory  directory;
    LocalSocket                   socket;
    std::vector<std::string_view> signal_order;

    socket.error_occurred.connect(
        [&](IOError, std::string const&) -> void { signal_order.emplace_back("error_occurred"); });
    socket.closed.connect([&]() -> void { signal_order.emplace_back("closed"); });
    socket.disconnected.connect([&]() -> void { signal_order.emplace_back("disconnected"); });

    auto const missing_path = directory.path() / "missing-secret.sock";
    socket.connect_to_server(missing_path);
    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == LocalSocketState::Unconnected; }));

    CHECK(socket.error() == IOError::OpenError);
    CHECK_FALSE(socket.error_string().empty());
    CHECK(socket.error_string().find("missing-secret") == std::string::npos);
    CHECK(signal_order == std::vector<std::string_view>{"error_occurred", "closed"});

    NativeLocalListener listener{directory.path() / "valid.sock"};
    REQUIRE(connect_socket(app, listener, socket));
    CHECK(socket.error() == IOError::NoError);
    CHECK(socket.error_string().empty());
}

TEST_CASE("LocalSocket connects and exposes Linux peer credentials", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    NativeLocalListener          listener{directory.path() / "credentials.sock"};
    LocalSocket                  socket;
    int                          connected_count       = 0;
    bool                         credentials_in_signal = false;

    socket.connected.connect([&]() -> void {
        ++connected_count;
        auto const& credentials = socket.peer_credentials();
        credentials_in_signal   = socket.state() == LocalSocketState::Connected &&
                                  credentials.process_id == static_cast<std::uint64_t>(::getpid()) &&
                                  credentials.user_id == static_cast<std::uint64_t>(::getuid()) &&
                                  credentials.group_id == static_cast<std::uint64_t>(::getgid());
    });

    REQUIRE(connect_socket(app, listener, socket));

    CHECK(socket.is_open());
    CHECK(socket.server_path() == listener.path());
    CHECK(socket.peer_credentials().process_id == static_cast<std::uint64_t>(::getpid()));
    CHECK(socket.peer_credentials().user_id == static_cast<std::uint64_t>(::getuid()));
    CHECK(socket.peer_credentials().group_id == static_cast<std::uint64_t>(::getgid()));
    CHECK(connected_count == 1);
    CHECK(credentials_in_signal);

    socket.close();
    CHECK(socket.state() == LocalSocketState::Unconnected);
    CHECK(socket.server_path() == listener.path());
    CHECK(socket.peer_credentials().process_id == static_cast<std::uint64_t>(::getpid()));
}

TEST_CASE("LocalSocket replacement aborts the old lifecycle and clears input", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    NativeLocalListener          first{directory.path() / "first.sock"};
    NativeLocalListener          second{directory.path() / "second.sock"};
    LocalSocket                  socket;
    int                          connected_count    = 0;
    int                          disconnected_count = 0;
    int                          closed_count       = 0;

    socket.connected.connect([&]() -> void { ++connected_count; });
    socket.disconnected.connect([&]() -> void { ++disconnected_count; });
    socket.closed.connect([&]() -> void { ++closed_count; });

    REQUIRE(connect_socket(app, first, socket));
    send_to_socket(app, first, socket, "stale", 5);
    REQUIRE(socket.bytes_available() == 5);

    socket.connect_to_server(second.path());
    CHECK(disconnected_count == 1);
    CHECK(closed_count == 1);
    CHECK(socket.bytes_available() == 0);
    CHECK(socket.server_path() == second.path());
    REQUIRE(wait_for(app, [&]() -> bool {
        auto const accepted = second.accept_client();
        return accepted && socket.state() == LocalSocketState::Connected;
    }));

    CHECK(connected_count == 2);
    CHECK(socket.peer_credentials().process_id == static_cast<std::uint64_t>(::getpid()));
}

TEST_CASE("LocalSocket preserves binary reads and line semantics", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    NativeLocalListener          listener{directory.path() / "reads.sock"};
    LocalSocket                  socket;

    REQUIRE(connect_socket(app, listener, socket));

    auto const binary = std::string{"a\0b", 3};
    send_to_socket(app, listener, socket, binary, binary.size());
    CHECK(socket.read(2) == std::string{"a\0", 2});
    CHECK(socket.read_all() == "b");
    CHECK(socket.bytes_available() == 0);

    send_to_socket(app, listener, socket, "\r\nabcdef\nlast", 13);
    CHECK(socket.can_read_line());
    CHECK(socket.read_line().empty());
    CHECK(socket.can_read_line());
    CHECK(socket.read_line(3) == "abc");
    CHECK(socket.read_line() == "def");
    CHECK_FALSE(socket.can_read_line());
    CHECK(socket.read_line().empty());

    listener.shutdown_writes();
    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == LocalSocketState::Unconnected; }));
    CHECK(socket.can_read_line());
    CHECK(socket.read_line() == "last");
    CHECK_FALSE(socket.can_read_line());
}

TEST_CASE("LocalSocket emits final read and close signals in order on EOF", "[net][local-socket]")
{
    Application                   app{0, nullptr};
    jb::test::TemporaryDirectory  directory;
    NativeLocalListener           listener{directory.path() / "eof.sock"};
    LocalSocket                   socket;
    std::vector<std::string_view> signal_order;

    socket.ready_read.connect([&]() -> void { signal_order.emplace_back("ready_read"); });
    socket.disconnected.connect([&]() -> void { signal_order.emplace_back("disconnected"); });
    socket.error_occurred.connect(
        [&](IOError, std::string const&) -> void { signal_order.emplace_back("error_occurred"); });
    socket.closed.connect([&]() -> void { signal_order.emplace_back("closed"); });

    REQUIRE(connect_socket(app, listener, socket));
    listener.queue_to_client("final");
    REQUIRE(listener.flush_to_client());
    listener.shutdown_writes();

    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == LocalSocketState::Unconnected; }));
    CHECK(signal_order == std::vector<std::string_view>{"ready_read", "disconnected", "closed"});
    CHECK(socket.error() == IOError::NoError);
    CHECK(socket.read_all() == "final");

    socket.close();
    socket.abort();
    CHECK(signal_order == std::vector<std::string_view>{"ready_read", "disconnected", "closed"});
}

TEST_CASE("LocalSocket enforces and rearms finite read-buffer limits", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    NativeLocalListener          listener{directory.path() / "backpressure.sock"};
    LocalSocket                  socket;

    socket.set_read_buffer_limit(4);
    REQUIRE(connect_socket(app, listener, socket));
    listener.queue_to_client("abcdefghijkl");
    REQUIRE(listener.flush_to_client());

    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 4; }));
    CHECK(socket.read(2) == "ab");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 4; }));
    CHECK(socket.read_all() == "cdef");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 4; }));
    CHECK(socket.read_all() == "ghij");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 2; }));
    CHECK(socket.read_all() == "kl");

    listener.queue_to_client("mnopqrst");
    REQUIRE(listener.flush_to_client());
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 4; }));
    socket.set_read_buffer_limit(2);
    app.process_events(EventFlag::All, 50);
    CHECK(socket.bytes_available() == 4);
    CHECK(socket.read(3) == "mno");
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 2; }));
    CHECK(socket.read_all() == "pq");
    socket.set_read_buffer_limit(0);
    REQUIRE(wait_for(app, [&]() -> bool { return socket.bytes_available() == 3; }));
    CHECK(socket.read_all() == "rst");
}

TEST_CASE("LocalSocket writes ordered binary data and extends a graceful flush", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    NativeLocalListener          listener{directory.path() / "writes.sock"};
    LocalSocket                  socket;
    std::size_t                  bytes_written = 0;

    socket.bytes_written.connect([&](std::size_t bytes) -> void { bytes_written += bytes; });
    REQUIRE(connect_socket(app, listener, socket));

    auto payload = std::string(4U * 1024U * 1024U, 'x');
    payload[17]  = '\0';
    CHECK(socket.write(payload) == payload.size());
    socket.disconnect_from_server();
    REQUIRE(socket.state() == LocalSocketState::Closing);
    CHECK(socket.write("tail") == 4);

    REQUIRE(wait_for(
        app,
        [&]() -> bool {
            auto const& received = listener.read_from_client();
            return received.size() == payload.size() + 4U && listener.peer_eof() &&
                   socket.state() == LocalSocketState::Unconnected;
        },
        std::chrono::seconds{8}));

    auto expected = payload + "tail";
    CHECK(listener.read_from_client() == expected);
    CHECK(bytes_written == expected.size());
    CHECK(socket.error() == IOError::NoError);
}

TEST_CASE("LocalSocket graceful close preserves input and abort discards it", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;

    SECTION("graceful close")
    {
        NativeLocalListener listener{directory.path() / "graceful.sock"};
        LocalSocket         socket;
        REQUIRE(connect_socket(app, listener, socket));
        send_to_socket(app, listener, socket, "keep", 4);

        socket.close();
        CHECK(socket.state() == LocalSocketState::Unconnected);
        CHECK(socket.read_all() == "keep");
    }

    SECTION("abort")
    {
        NativeLocalListener listener{directory.path() / "abort.sock"};
        LocalSocket         socket;
        int                 disconnected_count = 0;
        int                 closed_count       = 0;

        socket.disconnected.connect([&]() -> void { ++disconnected_count; });
        socket.closed.connect([&]() -> void { ++closed_count; });
        REQUIRE(connect_socket(app, listener, socket));
        send_to_socket(app, listener, socket, "discard", 7);

        socket.abort();
        CHECK(socket.state() == LocalSocketState::Unconnected);
        CHECK(socket.bytes_available() == 0);
        CHECK(disconnected_count == 1);
        CHECK(closed_count == 1);

        socket.abort();
        CHECK(disconnected_count == 1);
        CHECK(closed_count == 1);
    }
}

TEST_CASE("LocalSocket reports native write failures in lifecycle order", "[net][local-socket]")
{
    Application                   app{0, nullptr};
    jb::test::TemporaryDirectory  directory;
    NativeLocalListener           listener{directory.path() / "write-error.sock"};
    LocalSocket                   socket;
    std::vector<std::string_view> signal_order;

    socket.disconnected.connect([&]() -> void { signal_order.emplace_back("disconnected"); });
    socket.error_occurred.connect(
        [&](IOError, std::string const&) -> void { signal_order.emplace_back("error_occurred"); });
    socket.closed.connect([&]() -> void { signal_order.emplace_back("closed"); });

    REQUIRE(connect_socket(app, listener, socket));
    listener.reset_client();
    CHECK(socket.write("trigger") == 7);
    REQUIRE(wait_for(app, [&]() -> bool { return socket.state() == LocalSocketState::Unconnected; }));

    CHECK(socket.error() == IOError::WriteError);
    CHECK(signal_order == std::vector<std::string_view>{"disconnected", "error_occurred", "closed"});
}

TEST_CASE("LocalSocket destruction is signal-free and releases the peer", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    auto const                   path               = directory.path() / "destruction.sock";
    int                          disconnected_count = 0;
    int                          closed_count       = 0;

    {
        NativeLocalListener listener{path};
        {
            LocalSocket socket;
            socket.disconnected.connect([&]() -> void { ++disconnected_count; });
            socket.closed.connect([&]() -> void { ++closed_count; });
            REQUIRE(connect_socket(app, listener, socket));
        }

        REQUIRE(wait_for(app, [&]() -> bool {
            static_cast<void>(listener.read_from_client());
            return listener.peer_eof();
        }));
        CHECK(disconnected_count == 0);
        CHECK(closed_count == 0);
    }

    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("LocalSocket repeated lifecycles clean up descriptors and paths", "[net][local-socket]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalSocket                  socket;
    int                          closed_count = 0;

    socket.closed.connect([&]() -> void { ++closed_count; });

    for (int index = 0; index < 12; ++index) {
        auto const path = directory.path() / ("cycle-" + std::to_string(index) + ".sock");
        {
            NativeLocalListener listener{path};
            REQUIRE(connect_socket(app, listener, socket));
            socket.abort();
            CHECK(socket.state() == LocalSocketState::Unconnected);
        }
        CHECK_FALSE(std::filesystem::exists(path));
    }

    CHECK(closed_count == 12);
}

// NOLINTEND(readability-magic-numbers)
