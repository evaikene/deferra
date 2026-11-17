#pragma once

#include <jb/core/byte_buffer.hpp>
#include <jb/core/event_loop.hpp>
#include <jb/core/event_loop_types.hpp>
#include <jb/core/object.hpp>
#include <jb/core/process.hpp>
#include <jb/core/signal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono> // IWYU pragma: keep duration literals
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace jobu_php_test {
using jb::core::Clock;
using jb::core::EventLoop;
using namespace std::chrono_literals;

// The daemon lives on a dormant host loop while PHP attaches each request loop.
// Its Process owner reaps the child even if a test assertion interrupts the case.
class Daemon {
public:
    auto
    start(EventLoop& loop, std::filesystem::path const& directory, bool execute_cli = false, bool cron_default = false)
        -> void
    {
        _process.standard_error.connect(&_receiver, [this](jb::core::ByteBuffer const& bytes) {
            _log.append(jb::core::as_string_view(bytes));
        });
        _process.finished.connect(&_receiver, [this](jb::core::ProcessExit const& value) { _exit = value; });
        std::vector<std::string> arguments{"--no-config",
                                           "--database",
                                           (directory / "daemon.sqlite").string(),
                                           "--socket",
                                           (directory / "daemon.sock").string()};
        if (cron_default) {
            auto          config = directory / "cron.conf";
            std::ofstream stream(config);
            stream << "schedule.default_timezone = Europe/Tallinn\n";
            stream.close();
            arguments.erase(arguments.begin());
            arguments.emplace_back("--config");
            arguments.push_back(config.string());
        }
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-daemon");
            if (execute_cli) {
                // Root daemon startup does not authorize child execution. Only
                // explicitly opted-in, disposable test environments may do both.
                auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
                INFO("Root CLI execution requires JOBU_TEST_ALLOW_ROOT_CLI=1 in an isolated test environment");
                REQUIRE((enabled && std::string_view{enabled} == "1"));
                arguments.emplace_back("--allow-root-cli");
            }
        }
        REQUIRE(_process.start({.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments)}));
        REQUIRE(loop.run_until(Clock::now() + 8s,
                               [this] { return _exit || _log.find("jobud.ready") != std::string::npos; }));
        INFO(_log);
        REQUIRE_FALSE(_exit);
    }

    auto stop(EventLoop& loop) -> void
    {
        REQUIRE(_process.process_id());
        REQUIRE(::kill(static_cast<pid_t>(*_process.process_id()), SIGTERM) == 0);
        REQUIRE(loop.run_until(Clock::now() + 8s, [this] { return _exit.has_value(); }));
        INFO(_log);
        CHECK(_exit->kind == jb::core::ProcessExitKind::Exited);
        CHECK(_exit->exit_code == 0);
    }

    /// Captured daemon stderr, complete after stop() has reaped the process.
    auto log() const -> std::string const& { return _log; }

private:
    std::string                          _log;
    std::optional<jb::core::ProcessExit> _exit;
    jb::core::Object                     _receiver;
    jb::core::Process                    _process;
};

} // namespace jobu_php_test
