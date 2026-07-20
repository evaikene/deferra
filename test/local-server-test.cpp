#include "application.hpp"
#include "local_server.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::net;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

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

auto connect_and_take(Application& app, LocalServer& server, LocalSocket& client) -> std::unique_ptr<LocalSocket>
{
    client.connect_to_server(server.server_path());
    REQUIRE(wait_for(app, [&]() -> bool {
        return client.state() == LocalSocketState::Connected && server.pending_connection_count() != 0;
    }));
    return server.take_next_connection();
}

void write_file(std::filesystem::path const& path, std::string_view contents)
{
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    REQUIRE(stream);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    REQUIRE(stream.good());
}

auto read_file(std::filesystem::path const& path) -> std::string
{
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

auto path_metadata(std::filesystem::path const& path) -> struct stat {
    struct stat metadata{};
    REQUIRE(::lstat(path.c_str(), &metadata) == 0);
    return metadata;

}

auto socket_mode(std::filesystem::path const& path) -> mode_t

{
    auto const metadata = path_metadata(path);
    REQUIRE(S_ISSOCK(metadata.st_mode));
    return metadata.st_mode & static_cast<mode_t>(07777);
}

void check_binary_exchange(Application&     app,
                           LocalSocket&     client,
                           LocalSocket&     accepted,
                           std::string_view client_payload,
                           std::string_view server_payload)
{
    auto const client_split = client_payload.size() / 2U;
    CHECK(client.write(client_payload.substr(0, client_split)) == client_split);
    CHECK(client.write(client_payload.substr(client_split)) == client_payload.size() - client_split);
    REQUIRE(wait_for(app, [&]() -> bool { return accepted.bytes_available() == client_payload.size(); }));
    CHECK(accepted.read_all() == client_payload);

    auto const server_split = server_payload.size() / 2U;
    CHECK(accepted.write(server_payload.substr(0, server_split)) == server_split);
    CHECK(accepted.write(server_payload.substr(server_split)) == server_payload.size() - server_split);
    REQUIRE(wait_for(app, [&]() -> bool { return client.bytes_available() == server_payload.size(); }));
    CHECK(client.read_all() == server_payload);
}

} // anonymous namespace

TEST_CASE("LocalServer exposes defaults and requires an event loop", "[net][local-server]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "no-loop.sock";
    LocalServer                  server;
    int                          accept_error_count = 0;

    server.accept_error.connect([&](IOError, std::string const&) -> void { ++accept_error_count; });

    CHECK_FALSE(server.is_listening());
    CHECK(server.server_path().empty());
    CHECK(server.pending_connection_count() == 0);
    CHECK_FALSE(server.take_next_connection());
    CHECK(server.error() == IOError::NoError);
    CHECK(server.error_string().empty());

    CHECK_FALSE(server.listen(path));
    CHECK_FALSE(server.is_listening());
    CHECK(server.server_path() == path);
    CHECK(server.pending_connection_count() == 0);
    CHECK(server.error() == IOError::ResourceError);
    CHECK_FALSE(server.error_string().empty());
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(accept_error_count == 0);

    auto const error_message = server.error_string();
    server.close();
    server.close();
    CHECK(server.error() == IOError::ResourceError);
    CHECK(server.error_string() == error_message);
}

TEST_CASE("LocalServer cleans up when its initial watch registration fails", "[net][local-server]")
{
    jb::test::TemporaryDirectory           directory;
    auto                                   fake = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current_loop{fake.loop.get()};
    LocalServer                            server;
    int                                    accept_error_count = 0;
    auto const                             path               = directory.path() / "watch-failure.sock";

    server.accept_error.connect([&](IOError, std::string const&) -> void { ++accept_error_count; });
    fake.backend->add_fd_result = false;

    CHECK_FALSE(server.listen(path));
    CHECK(fake.backend->add_fd_calls == 1);
    REQUIRE(fake.backend->last_added_fd >= 0);
    errno = 0;
    CHECK(::fcntl(fake.backend->last_added_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK_FALSE(server.is_listening());
    CHECK(server.pending_connection_count() == 0);
    CHECK(server.error() == IOError::ResourceError);
    CHECK(server.error_string() == "local server event-loop watch registration failed");
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(accept_error_count == 0);
}

TEST_CASE("LocalServer discards an accepted socket when its first watch fails", "[net][local-server]")
{
    jb::test::TemporaryDirectory           directory;
    auto                                   fake = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current_loop{fake.loop.get()};
    LocalServer                            server;
    LocalSocket                            client;
    int                                    new_connection_count = 0;
    int                                    accept_error_count   = 0;
    IOError                                accept_error         = IOError::NoError;
    auto const                             path                 = directory.path() / "accepted-watch-failure.sock";

    server.new_connection.connect([&]() -> void { ++new_connection_count; });
    server.accept_error.connect([&](IOError error, std::string const&) -> void {
        ++accept_error_count;
        accept_error = error;
    });
    fake.backend->add_fd_results = {true, true, false};

    REQUIRE(server.listen(path));
    auto const listener_fd = fake.backend->last_added_fd;
    client.connect_to_server(path);
    REQUIRE(fake.backend->add_fd_calls == 2);

    fake.backend->ready_events.push_back({.fd = listener_fd, .events = FdEvent::Read});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);

    CHECK(fake.backend->add_fd_calls == 3);
    REQUIRE(fake.backend->last_added_fd >= 0);
    errno = 0;
    CHECK(::fcntl(fake.backend->last_added_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(server.is_listening());
    CHECK(server.pending_connection_count() == 0);
    CHECK(server.error() == IOError::ResourceError);
    CHECK(accept_error == IOError::ResourceError);
    CHECK(accept_error_count == 1);
    CHECK(new_connection_count == 0);
}

TEST_CASE("LocalServer rejects invalid paths and options without side effects", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    int                          accept_error_count = 0;

    server.accept_error.connect([&](IOError, std::string const&) -> void { ++accept_error_count; });

    auto check_invalid = [&](std::filesystem::path const& path, LocalServerOptions options = {}) -> void {
        CHECK_FALSE(server.listen(path, options));
        CHECK_FALSE(server.is_listening());
        CHECK(server.server_path() == path);
        CHECK(server.pending_connection_count() == 0);
        CHECK(server.error() == IOError::InvalidArgument);
        CHECK_FALSE(server.error_string().empty());
        if (!path.empty() && path.native().find('\0') == std::string::npos) {
            std::error_code ignored;
            CHECK_FALSE(std::filesystem::exists(path, ignored));
        }
    };

    check_invalid({});
    check_invalid(std::filesystem::path{
        std::string{"hidden\0path", 11}
    });
    check_invalid(directory.path() / std::string(256, 'x'));

    auto options    = LocalServerOptions{};
    options.backlog = 0;
    check_invalid(directory.path() / "zero-backlog.sock", options);
    options.backlog = -1;
    check_invalid(directory.path() / "negative-backlog.sock", options);

    options                         = {};
    options.max_pending_connections = 0;
    check_invalid(directory.path() / "zero-pending.sock", options);

    options             = {};
    options.permissions = std::filesystem::perms::unknown;
    check_invalid(directory.path() / "unknown-permissions.sock", options);

    using PermissionBits = std::underlying_type_t<std::filesystem::perms>;
    auto const mask      = static_cast<PermissionBits>(std::filesystem::perms::mask);
    options.permissions  = static_cast<std::filesystem::perms>(mask | static_cast<PermissionBits>(1U << 20U));
    check_invalid(directory.path() / "invalid-permissions.sock", options);

    CHECK(accept_error_count == 0);
}

TEST_CASE("LocalServer preserves existing paths and an active listener", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    int                          accept_error_count = 0;
    auto const                   regular_path       = directory.path() / "existing-file";
    auto const                   contents           = std::string{"preserve\0contents", 17};

    server.accept_error.connect([&](IOError, std::string const&) -> void { ++accept_error_count; });
    write_file(regular_path, contents);
    CHECK_FALSE(server.listen(regular_path));
    CHECK(server.error() == IOError::OpenError);
    CHECK(read_file(regular_path) == contents);

    auto const missing_parent_path = directory.path() / "missing" / "server.sock";
    CHECK_FALSE(server.listen(missing_parent_path));
    CHECK(server.error() == IOError::OpenError);
    CHECK_FALSE(std::filesystem::exists(missing_parent_path));

    auto const active_path = directory.path() / "active.sock";
    REQUIRE(server.listen(active_path));
    auto const active_metadata = path_metadata(active_path);

    LocalServer duplicate;
    CHECK_FALSE(duplicate.listen(active_path));
    CHECK(duplicate.error() == IOError::OpenError);
    CHECK(server.is_listening());

    auto const replacement_attempt = directory.path() / "replacement.sock";
    CHECK_FALSE(server.listen(replacement_attempt));
    CHECK(server.error() == IOError::InvalidArgument);
    CHECK(server.server_path() == active_path);
    CHECK(server.is_listening());
    CHECK_FALSE(std::filesystem::exists(replacement_attempt));
    auto const current_metadata = path_metadata(active_path);
    CHECK(current_metadata.st_dev == active_metadata.st_dev);
    CHECK(current_metadata.st_ino == active_metadata.st_ino);

    LocalSocket client;
    auto        accepted = connect_and_take(app, server, client);
    REQUIRE(accepted);
    check_binary_exchange(app, client, *accepted, "still-live", "confirmed");
    CHECK(accept_error_count == 0);
}

TEST_CASE("LocalServer applies permissions before exposing a listener", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;

    auto const default_path = directory.path() / "default-mode.sock";
    REQUIRE(server.listen(default_path));
    CHECK(server.is_listening());
    CHECK(server.server_path() == default_path);
    CHECK(server.pending_connection_count() == 0);
    CHECK(server.error() == IOError::NoError);
    CHECK(server.error_string().empty());
    CHECK(socket_mode(default_path) == 0600);
    server.close();
    CHECK_FALSE(server.is_listening());
    CHECK(server.server_path() == default_path);
    CHECK_FALSE(std::filesystem::exists(default_path));

    LocalServerOptions options;
    options.permissions =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
    options.accepted_read_buffer_limit = 0;
    auto const custom_path             = directory.path() / "custom-mode.sock";
    REQUIRE(server.listen(custom_path, options));
    CHECK(socket_mode(custom_path) == 0640);
    CHECK(server.error() == IOError::NoError);

    LocalSocket client;
    auto        accepted = connect_and_take(app, server, client);
    REQUIRE(accepted);
    CHECK(accepted->read_buffer_limit() == 0);
    server.close();
    CHECK_FALSE(std::filesystem::exists(custom_path));

    options.permissions       = std::filesystem::perms::none;
    auto const no_access_path = directory.path() / "no-access-mode.sock";
    REQUIRE(server.listen(no_access_path, options));
    CHECK(socket_mode(no_access_path) == 0);
    server.close();
    CHECK_FALSE(std::filesystem::exists(no_access_path));
}

TEST_CASE("LocalServer exchanges binary data through accepted LocalSockets", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    LocalSocket                  client;
    int                          accepted_connected_count = 0;

    LocalServerOptions options;
    options.accepted_read_buffer_limit = 37;
    REQUIRE(server.listen(directory.path() / "exchange.sock", options));
    auto accepted = connect_and_take(app, server, client);
    REQUIRE(accepted);

    accepted->connected.connect([&]() -> void { ++accepted_connected_count; });
    app.process_events(EventFlag::All, 20);

    CHECK(accepted->state() == LocalSocketState::Connected);
    CHECK(accepted->is_open());
    CHECK(accepted->server_path() == server.server_path());
    CHECK(accepted->read_buffer_limit() == options.accepted_read_buffer_limit);
    REQUIRE(accepted->peer_credentials().process_id);
    REQUIRE(accepted->peer_credentials().user_id);
    REQUIRE(accepted->peer_credentials().group_id);
    CHECK(*accepted->peer_credentials().process_id == static_cast<std::uint64_t>(::getpid()));
    CHECK(*accepted->peer_credentials().user_id == static_cast<std::uint64_t>(::getuid()));
    CHECK(*accepted->peer_credentials().group_id == static_cast<std::uint64_t>(::getgid()));
    CHECK(accepted_connected_count == 0);

    auto const client_payload = std::string{"client\0binary-data", 18};
    auto const server_payload = std::string{"server-data\0reply", 17};
    check_binary_exchange(app, client, *accepted, client_payload, server_payload);
}

TEST_CASE("LocalServer queues multiple clients in FIFO order", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    int                          new_connection_count = 0;
    auto const                   path                 = directory.path() / "fifo.sock";

    server.new_connection.connect([&]() -> void { ++new_connection_count; });
    REQUIRE(server.listen(path));

    std::vector<std::unique_ptr<LocalSocket>> clients;
    std::vector<std::string>                  markers{"first", "second", "third"};
    for (std::size_t index = 0; index < markers.size(); ++index) {
        clients.push_back(std::make_unique<LocalSocket>());
        clients.back()->connect_to_server(path);
    }

    REQUIRE(wait_for(app, [&]() -> bool {
        auto const all_connected = std::ranges::all_of(clients, [](auto const& client) -> bool {
            return client->state() == LocalSocketState::Connected;
        });
        return all_connected && server.pending_connection_count() == clients.size();
    }));
    CHECK(new_connection_count >= 1);
    CHECK(new_connection_count <= static_cast<int>(clients.size()));

    std::vector<std::unique_ptr<LocalSocket>> accepted;
    for (std::size_t index = 0; index < clients.size(); ++index) {
        accepted.push_back(server.take_next_connection());
        REQUIRE(accepted.back());
        CHECK(clients[index]->write(markers[index]) == markers[index].size());
    }
    CHECK(server.pending_connection_count() == 0);
    CHECK_FALSE(server.take_next_connection());

    REQUIRE(wait_for(app, [&]() -> bool {
        for (std::size_t index = 0; index < accepted.size(); ++index) {
            if (accepted[index]->bytes_available() != markers[index].size()) {
                return false;
            }
        }
        return true;
    }));
    for (std::size_t index = 0; index < accepted.size(); ++index) {
        CHECK(accepted[index]->read_all() == markers[index]);
    }
}

TEST_CASE("LocalServer resumes acceptance after the pending limit is drained", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    LocalSocket                  first_client;
    LocalSocket                  second_client;
    LocalServerOptions           options;

    options.max_pending_connections    = 1;
    options.accepted_read_buffer_limit = 19;
    auto const path                    = directory.path() / "pending-limit.sock";
    REQUIRE(server.listen(path, options));

    first_client.connect_to_server(path);
    second_client.connect_to_server(path);
    REQUIRE(wait_for(app, [&]() -> bool {
        return first_client.state() == LocalSocketState::Connected &&
               second_client.state() == LocalSocketState::Connected && server.pending_connection_count() == 1;
    }));

    CHECK(first_client.write("first") == 5);
    CHECK(second_client.write("second") == 6);
    auto first = server.take_next_connection();
    REQUIRE(first);
    CHECK(first->read_buffer_limit() == options.accepted_read_buffer_limit);
    REQUIRE(wait_for(app, [&]() -> bool { return first->bytes_available() == 5; }));
    CHECK(first->read_all() == "first");

    REQUIRE(wait_for(app, [&]() -> bool { return server.pending_connection_count() == 1; }));
    auto second = server.take_next_connection();
    REQUIRE(second);
    CHECK(second->read_buffer_limit() == options.accepted_read_buffer_limit);
    REQUIRE(wait_for(app, [&]() -> bool { return second->bytes_available() == 6; }));
    CHECK(second->read_all() == "second");
    CHECK_FALSE(server.take_next_connection());
}

TEST_CASE("LocalServer close preserves transferred sockets and drops pending sockets", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;

    SECTION("transferred socket remains independent")
    {
        LocalServer server;
        LocalSocket client;
        auto const  path = directory.path() / "transferred.sock";
        REQUIRE(server.listen(path));
        auto accepted = connect_and_take(app, server, client);
        REQUIRE(accepted);

        server.close();
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(accepted->is_open());
        check_binary_exchange(app, client, *accepted, "after-close", "still-open");
    }

    SECTION("pending socket is destroyed")
    {
        LocalServer server;
        LocalSocket client;
        auto const  path = directory.path() / "pending-close.sock";
        REQUIRE(server.listen(path));
        client.connect_to_server(path);
        REQUIRE(wait_for(app, [&]() -> bool { return server.pending_connection_count() == 1; }));

        server.close();
        CHECK(server.pending_connection_count() == 0);
        CHECK_FALSE(std::filesystem::exists(path));
        REQUIRE(wait_for(app, [&]() -> bool { return client.state() == LocalSocketState::Unconnected; }));
    }
}

TEST_CASE("LocalServer permits reentrant close from new_connection", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;
    LocalSocket                  client;
    int                          new_connection_count = 0;
    auto const                   path                 = directory.path() / "reentrant-close.sock";

    server.new_connection.connect([&]() -> void {
        ++new_connection_count;
        server.close();
    });
    REQUIRE(server.listen(path));
    client.connect_to_server(path);

    REQUIRE(wait_for(app, [&]() -> bool { return new_connection_count == 1 && !server.is_listening(); }));
    CHECK(server.pending_connection_count() == 0);
    CHECK_FALSE(std::filesystem::exists(path));
    REQUIRE(wait_for(app, [&]() -> bool { return client.state() == LocalSocketState::Unconnected; }));
}

TEST_CASE("LocalServer destruction and close clean up only their own socket entry", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    auto const                   destructor_path      = directory.path() / "destructor.sock";
    int                          new_connection_count = 0;
    int                          accept_error_count   = 0;

    {
        LocalServer server;
        server.new_connection.connect([&]() -> void { ++new_connection_count; });
        server.accept_error.connect([&](IOError, std::string const&) -> void { ++accept_error_count; });
        REQUIRE(server.listen(destructor_path));
    }
    CHECK_FALSE(std::filesystem::exists(destructor_path));
    CHECK(new_connection_count == 0);
    CHECK(accept_error_count == 0);

    auto const  replacement_path = directory.path() / "replacement.sock";
    LocalServer server;
    REQUIRE(server.listen(replacement_path));
    REQUIRE(std::filesystem::remove(replacement_path));
    auto const replacement_contents = std::string{"replacement\0data", 16};
    write_file(replacement_path, replacement_contents);

    server.close();
    CHECK(read_file(replacement_path) == replacement_contents);
    CHECK(server.error() == IOError::NoError);
}

TEST_CASE("LocalServer clears errors on success and supports repeated lifecycles", "[net][local-server]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    LocalServer                  server;

    auto const missing_path = directory.path() / "missing" / "failure.sock";
    CHECK_FALSE(server.listen(missing_path));
    CHECK(server.error() == IOError::OpenError);
    CHECK_FALSE(server.error_string().empty());

    for (int index = 0; index < 8; ++index) {
        auto const path = directory.path() / ("cycle-" + std::to_string(index) + ".sock");
        REQUIRE(server.listen(path));
        CHECK(server.error() == IOError::NoError);
        CHECK(server.error_string().empty());

        LocalSocket client;
        auto        accepted = connect_and_take(app, server, client);
        REQUIRE(accepted);
        server.close();
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(accepted->is_open());

        accepted.reset();
        REQUIRE(wait_for(app, [&]() -> bool { return client.state() == LocalSocketState::Unconnected; }));
        CHECK(server.pending_connection_count() == 0);
    }
}

// NOLINTEND(readability-magic-numbers)
