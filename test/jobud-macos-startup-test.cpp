#include "application.hpp"
#include "byte_buffer.hpp"
#include "process.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace jb::core;
using namespace std::chrono_literals;

template <typename Predicate>
void until(Application& app, Predicate predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(app.process_events(EventFlag::All, 20) != ProcessEventsResult::Failed);
    }
    REQUIRE(predicate());
}

void check_private_artifact(std::filesystem::path const& path, bool socket = false)
{
    CAPTURE(path);
    struct stat metadata{};
    REQUIRE(::lstat(path.c_str(), &metadata) == 0);
    CHECK(metadata.st_uid == ::geteuid());
    CHECK((metadata.st_mode & 07777) == 0600);
    if (socket) {
        CHECK(S_ISSOCK(metadata.st_mode));
    }
    else {
        CHECK(S_ISREG(metadata.st_mode));
    }
}

} // namespace

TEST_CASE("macOS foreground daemon protects new state and restarts after a normal stop", "[jobud][macos][startup]")
{
    if (::geteuid() == 0) {
        SKIP("macOS developer startup requires an ordinary-user test process");
    }

    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   database = directory.path() / "daemon.sqlite";
    auto const                   socket   = directory.path() / "daemon.sock";
    Application                  app{0, nullptr};

    // Reuse the same database and endpoint after a clean stop. Process is destroyed
    // before its callback captures, including when an assertion aborts the test.
    for (int incarnation = 0; incarnation < 2; ++incarnation) {
        CAPTURE(incarnation);
        std::string                log;
        std::optional<ProcessExit> exit;
        Process                    daemon;
        daemon.standard_error.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        daemon.finished.connect(&app, [&](ProcessExit const& value) { exit = value; });

        // Set only the child's inherited umask, without changing Catch's process-wide
        // state. Positional shell arguments preserve paths without interpolation.
        REQUIRE(daemon.start({
            .executable = "/bin/sh",
            .arguments  = {"-c",
                           "umask 022; exec \"$@\"", "jobud-startup-test",
                           JOBUD_EXECUTABLE, "--no-config",
                           "--database", database.string(),
                           "--socket", socket.string()},
            .timeout    = 15s
        }));
        until(app, [&] { return exit || log.find("jobud.ready") != std::string::npos; });
        INFO(log);
        REQUIRE_FALSE(exit);

        // Inspect sidecars while SQLite is open; a clean close may remove them.
        for (auto const* suffix : {"", "-wal", "-shm", ".lock"}) {
            check_private_artifact(database.string() + suffix);
        }
        check_private_artifact(socket, true);

        REQUIRE(daemon.process_id());
        REQUIRE(::kill(static_cast<pid_t>(*daemon.process_id()), SIGTERM) == 0);
        until(app, [&] { return exit.has_value(); });
        CHECK(exit->kind == ProcessExitKind::Exited);
        CHECK(exit->exit_code == 0);
        CHECK_FALSE(exit->stderr_lost);
        CHECK_FALSE(std::filesystem::exists(socket));
        check_private_artifact(database);
    }
}
