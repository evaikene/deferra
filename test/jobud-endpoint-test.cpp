#include "endpoint_guard_priv.hpp"

#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

using namespace jb::jobud::detail;

struct EndpointFixture {
    EndpointFixture()
        : directory{std::filesystem::perms::owner_all}
        , path{directory.path() / "daemon.sock"}
        , lock_path{path.string() + ".lock"}
    {
        auto opened = open_trusted_parent(path, ::geteuid(), false);
        REQUIRE(opened);
        REQUIRE(*opened);
        parent = std::move(**opened);
    }

    auto acquire() -> EndpointGuard
    {
        auto acquired = EndpointGuard::acquire(*parent, ::geteuid());
        REQUIRE(acquired);
        return std::move(*acquired);
    }

    auto bind_socket(bool listening = false) const -> PathDescriptor
    {
        PathDescriptor socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        REQUIRE(socket.get() >= 0);
        sockaddr_un address{};
        auto const  name = path.string();
        REQUIRE(name.size() < sizeof(address.sun_path));
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        auto const length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
        REQUIRE(::bind(socket.get(), reinterpret_cast<sockaddr const*>(&address), length) == 0);
        if (listening) {
            REQUIRE(::listen(socket.get(), 1) == 0);
        }
        return socket;
    }

    jb::test::TemporaryDirectory directory;
    std::filesystem::path        path;
    std::filesystem::path        lock_path;
    std::optional<TrustedParent> parent;
};

auto metadata(std::filesystem::path const& path) -> struct stat {
    struct stat value{};
    REQUIRE(::lstat(path.c_str(), &value) == 0);
    return value;

}

auto descriptor_count() -> std::size_t

{
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

} // namespace

TEST_CASE("Endpoint lock persists and excludes another owner until guard destruction", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    ino_t           inode{};
    {
        auto guard = fixture.acquire();
        REQUIRE(guard.verify());
        auto const held = metadata(fixture.lock_path);
        inode           = held.st_ino;
        CHECK(S_ISREG(held.st_mode));
        CHECK((held.st_mode & 07777) == 0600);
        CHECK(held.st_uid == ::geteuid());
        bool found_descriptor{false};
        for (auto const& entry : std::filesystem::directory_iterator{"/proc/self/fd"}) {
            if (std::filesystem::read_symlink(entry.path()) == fixture.lock_path) {
                auto const descriptor = std::stoi(entry.path().filename().string());
                CHECK((::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0);
                found_descriptor = true;
            }
        }
        CHECK(found_descriptor);
        auto competing = EndpointGuard::acquire(*fixture.parent, ::geteuid());
        REQUIRE_FALSE(competing);
        CHECK(competing.error().code == "jobud.socket.in_use");
        CHECK(metadata(fixture.lock_path).st_ino == inode);
        CHECK(guard.prepare_socket());
    }
    CHECK(metadata(fixture.lock_path).st_ino == inode);
    auto next = fixture.acquire();
    CHECK(next.verify());
    CHECK(metadata(fixture.lock_path).st_ino == inode);
}

TEST_CASE("Endpoint lock rejects unsafe existing entries without repair", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    SECTION("symlink")
    {
        std::filesystem::create_symlink("missing-target", fixture.lock_path);
    }
    SECTION("directory")
    {
        std::filesystem::create_directory(fixture.lock_path);
    }
    SECTION("FIFO")
    {
        REQUIRE(::mkfifo(fixture.lock_path.c_str(), 0600) == 0);
    }
    SECTION("group-writable file")
    {
        std::ofstream{fixture.lock_path} << "preserve";
        std::filesystem::permissions(fixture.lock_path,
                                     std::filesystem::perms::owner_all | std::filesystem::perms::group_write);
    }
    auto const before   = metadata(fixture.lock_path);
    auto       acquired = EndpointGuard::acquire(*fixture.parent, ::geteuid());
    REQUIRE_FALSE(acquired);
    CHECK(acquired.error().code == "jobud.path.unsafe");
    auto const after = metadata(fixture.lock_path);
    CHECK(after.st_ino == before.st_ino);
    CHECK(after.st_mode == before.st_mode);
    CHECK(after.st_uid == before.st_uid);
}

TEST_CASE("Endpoint lock preserves an existing foreign-owned file", "[jobud][endpoint][root]")
{
    if (::geteuid() != 0) {
        SKIP("foreign-owner fixture requires an isolated root run");
    }
    EndpointFixture fixture;
    std::ofstream{fixture.lock_path} << "preserve";
    REQUIRE(::chmod(fixture.lock_path.c_str(), 0600) == 0);
    REQUIRE(::chown(fixture.lock_path.c_str(), 1, static_cast<gid_t>(-1)) == 0);

    auto const before   = metadata(fixture.lock_path);
    auto       acquired = EndpointGuard::acquire(*fixture.parent, ::geteuid());
    REQUIRE_FALSE(acquired);
    CHECK(acquired.error().code == "jobud.path.unsafe");
    auto const after = metadata(fixture.lock_path);
    CHECK(after.st_ino == before.st_ino);
    CHECK(after.st_mode == before.st_mode);
    CHECK(after.st_uid == before.st_uid);
}

TEST_CASE("Endpoint admission refuses lock pathname or runtime-parent replacement", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard = fixture.acquire();
    SECTION("lock replacement")
    {
        std::filesystem::rename(fixture.lock_path, fixture.directory.path() / "held.lock");
        auto other    = fixture.acquire();
        auto verified = guard.verify();
        REQUIRE_FALSE(verified);
        CHECK(verified.error().detail == "endpoint_lock_changed");
        CHECK(other.verify());
        CHECK_FALSE(guard.prepare_socket());
    }
    SECTION("lock symlink replacement")
    {
        std::filesystem::rename(fixture.lock_path, fixture.directory.path() / "held.lock");
        std::filesystem::create_symlink("held.lock", fixture.lock_path);
        CHECK_FALSE(guard.verify());
        CHECK_FALSE(guard.prepare_socket());
        CHECK(std::filesystem::is_symlink(fixture.lock_path));
    }
    SECTION("parent mode change")
    {
        std::filesystem::permissions(fixture.directory.path(),
                                     std::filesystem::perms::owner_all | std::filesystem::perms::group_write);
        CHECK_FALSE(guard.verify());
        CHECK_FALSE(guard.prepare_socket());
    }
}

TEST_CASE("Endpoint stale inspection preserves non-sockets and live listeners", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard = fixture.acquire();
    PathDescriptor  listener;
    SECTION("regular file")
    {
        std::ofstream{fixture.path} << "preserve";
    }
    SECTION("symlink")
    {
        std::filesystem::create_symlink("missing-target", fixture.path);
    }
    SECTION("directory")
    {
        std::filesystem::create_directory(fixture.path);
    }
    SECTION("live listener")
    {
        listener = fixture.bind_socket(true);
    }
    auto const before   = metadata(fixture.path);
    auto       prepared = guard.prepare_socket();
    REQUIRE_FALSE(prepared);
    CHECK(metadata(fixture.path).st_ino == before.st_ino);
    CHECK(metadata(fixture.path).st_mode == before.st_mode);
}

TEST_CASE("Endpoint stale inspection preserves a foreign-owned socket", "[jobud][endpoint][root]")
{
    if (::geteuid() != 0) {
        SKIP("foreign-owner fixture requires an isolated root run");
    }
    EndpointFixture fixture;
    auto            guard    = fixture.acquire();
    auto            listener = fixture.bind_socket();
    REQUIRE(::chown(fixture.path.c_str(), 1, static_cast<gid_t>(-1)) == 0);

    auto const before   = metadata(fixture.path);
    auto       prepared = guard.prepare_socket();
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error().code == "jobud.path.unsafe");
    auto const after = metadata(fixture.path);
    CHECK(after.st_ino == before.st_ino);
    CHECK(after.st_mode == before.st_mode);
    CHECK(after.st_uid == before.st_uid);
}

TEST_CASE("Endpoint removes a definitely refused socket and retains its lock", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard      = fixture.acquire();
    auto const      lock_inode = metadata(fixture.lock_path).st_ino;
    // Closing an un-listened bound socket leaves the same artifact as a crashed listener.
    {
        auto socket = fixture.bind_socket();
    }
    REQUIRE(guard.prepare_socket());
    CHECK_FALSE(std::filesystem::exists(fixture.path));
    CHECK(metadata(fixture.lock_path).st_ino == lock_inode);
    CHECK(guard.prepare_socket());
}

TEST_CASE("Endpoint probe errors preserve the socket and close the probe descriptor", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard       = fixture.acquire();
    auto            socket      = fixture.bind_socket();
    auto const      before      = metadata(fixture.path);
    auto const      descriptors = descriptor_count();
    for (int error : {EINPROGRESS, EALREADY, EAGAIN, EINTR, ETIMEDOUT, EACCES, ENOENT, EIO}) {
        INFO(error);
        auto prepared = guard.prepare_socket([error](int fd, sockaddr const*, socklen_t) {
            CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
            CHECK((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
            errno = error;
            return -1;
        });
        REQUIRE_FALSE(prepared);
        CHECK(metadata(fixture.path).st_ino == before.st_ino);
        CHECK(descriptor_count() == descriptors);
    }
}

TEST_CASE("Endpoint rechecks socket and lock identities after a refused probe", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard  = fixture.acquire();
    auto            socket = fixture.bind_socket();
    PathDescriptor  replacement;
    bool            replace_lock{false};
    SECTION("different socket inode")
    {}
    SECTION("different lock inode")
    {
        replace_lock = true;
    }
    auto prepared = guard.prepare_socket([&](int, sockaddr const*, socklen_t) {
        if (replace_lock) {
            std::filesystem::rename(fixture.lock_path, fixture.directory.path() / "held.lock");
            std::ofstream{fixture.lock_path} << "replacement";
            REQUIRE(::chmod(fixture.lock_path.c_str(), 0600) == 0);
        }
        else {
            // Keep the original inode allocated so the replacement cannot reuse its number.
            std::filesystem::rename(fixture.path, fixture.directory.path() / "old.sock");
            replacement = fixture.bind_socket();
        }
        errno = ECONNREFUSED;
        return -1;
    });
    REQUIRE_FALSE(prepared);
    CHECK(std::filesystem::is_socket(fixture.path));
    CHECK(std::filesystem::exists(fixture.lock_path));
}

TEST_CASE("Endpoint repeated acquisition failures leak no descriptors", "[jobud][endpoint]")
{
    EndpointFixture fixture;
    auto            guard       = fixture.acquire();
    auto const      descriptors = descriptor_count();
    for (unsigned attempt = 0; attempt != 20; ++attempt) {
        CHECK_FALSE(EndpointGuard::acquire(*fixture.parent, ::geteuid()));
    }
    CHECK(descriptor_count() == descriptors);
}
