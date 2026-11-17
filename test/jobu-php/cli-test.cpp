#include "daemon-fixture.hpp"
#include "support/temporary_directory.hpp"

#include <jb/core/scoped_event_loop.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace std::chrono_literals;
using jb::core::Clock;

// The driver links only the native Client SDK. The child loads the production
// module; no Zend fixture classes or Embed installation are required here.
auto run_cli(std::string const& scenario, bool execute_cli, bool cron_default) -> void
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&                        loop = host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    jobu_php_test::Daemon        daemon;
    daemon.start(loop, directory.path(), execute_cli, cron_default);

    jb::core::Process                    process;
    jb::core::Object                     receiver;
    std::optional<jb::core::ProcessExit> exit;
    std::string                          errors;
    std::string                          output;
    process.standard_output.connect(&receiver, [&](jb::core::ByteBuffer const& bytes) {
        output.append(jb::core::as_string_view(bytes));
    });
    process.standard_error.connect(&receiver, [&](jb::core::ByteBuffer const& bytes) {
        errors.append(jb::core::as_string_view(bytes));
    });
    process.finished.connect(&receiver, [&](jb::core::ProcessExit const& value) { exit = value; });
    REQUIRE(process.start({
        .executable = JOBU_PHP_BINARY,
        .arguments  = {"-n",
                       "-dextension=" JOBU_PHP_MODULE,
                       std::string{JOBU_PHP_SCENARIOS} + "/" + scenario + ".php",
                       (directory.path() / "daemon.sock").string()}
    }));
    REQUIRE(loop.run_until(Clock::now() + 30s, [&] { return exit.has_value(); }));
    INFO(errors);
    INFO(output);
    CHECK(errors.empty());
    CHECK(output.find("Warning:") == std::string::npos);
    CHECK(output.find("Fatal error:") == std::string::npos);
    CHECK(output.find("memory leaks detected") == std::string::npos);
    CHECK(exit->kind == jb::core::ProcessExitKind::Exited);
    CHECK(exit->exit_code == 0);
    daemon.stop(loop);
}
} // namespace

TEST_CASE("Production CLI scenarios use the same-build daemon without Embed", "[php][production-cli]")
{
    SECTION("client")
    {
        run_cli("client", false, false);
    }
    SECTION("list")
    {
        run_cli("list", false, false);
    }
    SECTION("history")
    {
        run_cli("history", true, false);
    }
    SECTION("output")
    {
        run_cli("output", true, false);
    }
    SECTION("secrets-schedules")
    {
        run_cli("secrets-schedules", false, true);
    }
    SECTION("statistics")
    {
        run_cli("statistics", false, true);
    }
    SECTION("queue-job-cli")
    {
        run_cli("queue-job-cli", false, false);
    }
}

TEST_CASE("Connected production objects survive use and cleanly shut down", "[php][connected-cli]")
{
    run_cli("connected-lifecycle", true, false);
}
