#include "application.hpp"
#include "local_server.hpp"
#include "local_server_setup_hooks.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct SetupScenario {
    std::filesystem::path path;
    gid_t                 group{};
    bool                  fail_group{false};
    bool                  skip_group{false};
    bool                  skip_permissions{false};
    bool                  replace_path{false};
    bool                  group_applied{false};
    bool                  permissions_applied{false};
    unsigned              listen_calls{};
};

SetupScenario scenario;

auto descriptor_count() -> std::size_t
{
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

} // namespace

namespace jb::test {

auto local_server_group(std::filesystem::path const& path, gid_t group) -> int
{
    CHECK_FALSE(scenario.permissions_applied);
    if (scenario.fail_group) {
        errno = EPERM;
        return -1;
    }
    if (scenario.skip_group) {
        return 0;
    }
    auto const result      = ::lchown(path.c_str(), static_cast<uid_t>(-1), group);
    scenario.group_applied = result == 0;
    return result;
}

auto local_server_permissions(std::filesystem::path const& path, mode_t mode) -> int
{
    if (scenario.skip_permissions) {
        return 0;
    }
    auto const result            = ::chmod(path.c_str(), mode);
    scenario.permissions_applied = result == 0;
    if (scenario.replace_path) {
        // Replacement occurs after binding, at the metadata verification boundary.
        std::filesystem::rename(path, path.string() + ".owned");
        std::filesystem::create_symlink("preserve-target", path);
    }
    return result;
}

auto local_server_listen(int descriptor, int backlog) -> int
{
    ++scenario.listen_calls;
    CHECK(scenario.group_applied);
    CHECK(scenario.permissions_applied);
    struct stat metadata{};
    REQUIRE(::lstat(scenario.path.c_str(), &metadata) == 0);
    CHECK(metadata.st_gid == scenario.group);
    CHECK((metadata.st_mode & 07777) == 0660);

    // The final ownership is already visible, but this socket cannot accept a connection yet.
    auto const probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(probe >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto const name    = scenario.path.string();
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    auto const result = ::connect(probe, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
    auto const error  = errno;
    static_cast<void>(::close(probe));
    CHECK(result == -1);
    CHECK(error == ECONNREFUSED);
    return ::listen(descriptor, backlog);
}

} // namespace jb::test

TEST_CASE("Listener verifies group and mode before the native listen call", "[net][local-server][ownership]")
{
    jb::core::Application        app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    scenario = {.path = directory.path() / "daemon.sock", .group = ::getegid()};
    jb::net::LocalServer server;
    auto                 options = jb::net::LocalServerOptions{};
    options.group_id             = scenario.group;
    options.permissions          = static_cast<std::filesystem::perms>(0660);
    REQUIRE(server.listen(scenario.path, options));
    CHECK(scenario.listen_calls == 1);
    server.close();
    CHECK_FALSE(std::filesystem::exists(scenario.path));
}

TEST_CASE("Listener ownership failures do not listen or leak their bound descriptor", "[net][local-server][ownership]")
{
    jb::core::Application        app{0, nullptr};
    jb::test::TemporaryDirectory directory;
    scenario            = {.path = directory.path() / "daemon.sock", .group = ::getegid()};
    auto options        = jb::net::LocalServerOptions{};
    options.group_id    = scenario.group;
    options.permissions = std::filesystem::perms::none;
    SECTION("group assignment failure")
    {
        scenario.fail_group = true;
    }
    SECTION("unapplied group")
    {
        scenario.skip_group = true;
        options.group_id    = static_cast<unsigned long long>(::getegid()) + 1;
    }
    SECTION("unapplied permissions")
    {
        scenario.skip_permissions = true;
    }
    SECTION("replacement symlink")
    {
        scenario.replace_path = true;
    }
    auto const           descriptors = descriptor_count();
    jb::net::LocalServer server;
    REQUIRE_FALSE(server.listen(scenario.path, options));
    CHECK_FALSE(server.is_listening());
    CHECK(scenario.listen_calls == 0);
    CHECK(descriptor_count() == descriptors);
    if (scenario.replace_path) {
        CHECK(std::filesystem::is_symlink(scenario.path));
    }
    else {
        CHECK_FALSE(std::filesystem::exists(scenario.path));
    }
}
