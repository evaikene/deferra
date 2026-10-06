#include "application.hpp"
#include "byte_buffer.hpp"
#include "privileges_priv.hpp"
#include "process.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono> // IWYU pragma: keep duration literals in process requests.
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace jb::core;
using namespace std::chrono_literals;

struct CapturedProcess {
    ProcessExit exit;
    std::string output;
};

auto run_child(std::string executable, std::vector<std::string> arguments) -> CapturedProcess
{
    // Credential changes happen after exec in a separate process; Catch and its collaborators keep their identity.
    Application                app{0, nullptr};
    Process                    process;
    std::optional<ProcessExit> exit;
    std::string                output;
    process.standard_output.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
    process.standard_error.connect(&app, [&](ByteBuffer const& bytes) { output.append(as_string_view(bytes)); });
    process.finished.connect(&app, [&](ProcessExit const& value) {
        exit = value;
        app.quit();
    });
    REQUIRE(process.start({.executable = std::move(executable), .arguments = std::move(arguments), .timeout = 10s}));
    static_cast<void>(app.exec());
    INFO(output);
    REQUIRE(exit);
    REQUIRE(exit->kind == ProcessExitKind::Exited);
    REQUIRE_FALSE(exit->stdout_lost);
    REQUIRE_FALSE(exit->stderr_lost);
    return {.exit = *exit, .output = std::move(output)};
}

auto native_account() -> std::string
{
    auto const* found = ::getpwuid(::getuid());
    REQUIRE(found != nullptr);
    return found->pw_name;
}

auto privileged_target() -> std::string
{
    // Only explicitly provisioned disposable identities are used. Never select an arbitrary host account.
    auto const* user = std::getenv("JOBU_TEST_PRIVILEGE_USER");
    if (::geteuid() != 0 || user == nullptr) {
        SKIP("native root drop requires root and JOBU_TEST_PRIVILEGE_USER in a disposable environment");
    }
    auto operations = jb::jobud::detail::make_system_privilege_operations();
    auto account    = operations->account(user);
    REQUIRE(account);
    REQUIRE(account->user != 0);
    return user;
}

} // namespace

TEST_CASE("native non-root helpers preserve identity and reject another account", "[jobud][identity][native]")
{
    if (::geteuid() == 0) {
        SKIP("ordinary non-root identity evidence requires a non-root test process");
    }
    auto const parent_user  = ::geteuid();
    auto const parent_group = ::getegid();
    auto const preserve     = run_child(JOBUD_IDENTITY_TEST_HELPER, {"preserve"});
    CHECK(preserve.exit.exit_code == 0);
    CHECK(preserve.output.find("verified uid=") != std::string::npos);
    auto const same = run_child(JOBUD_IDENTITY_TEST_HELPER, {"target", native_account()});
    CHECK(same.exit.exit_code == 0);
    auto const different = run_child(JOBUD_IDENTITY_TEST_HELPER, {"target", "root"});
    CHECK(different.exit.exit_code == 1);
    CHECK(different.output.find("jobud.privilege.identity_mismatch") != std::string::npos);
    auto const different_group = run_child(JOBUD_IDENTITY_TEST_HELPER, {"target", native_account(), "root"});
    CHECK(different_group.exit.exit_code == 1);
    CHECK(different_group.output.find("jobud.privilege.identity_mismatch") != std::string::npos);
    CHECK(::geteuid() == parent_user);
    CHECK(::getegid() == parent_group);
}

TEST_CASE("non-root daemon refusal precedes resource creation", "[jobud][identity][native]")
{
    if (::geteuid() == 0) {
        SKIP("native non-root daemon refusal requires a non-root test process");
    }
    jb::test::TemporaryDirectory directory;
    auto const                   result = run_child(JOBUD_EXECUTABLE,
                                                    {"--no-config",
                                                     "--run-as-user",
                                                     "root",
                                                     "--allow-root-daemon",
                                                     "--allow-root-cli",
                                                     "--database",
                                                     (directory.path() / "unused.sqlite").string(),
                                                     "--socket",
                                                     (directory.path() / "unused.sock").string()});
    CHECK(result.exit.exit_code == 1);
    CHECK(result.output.find("jobud.privilege.identity_mismatch") != std::string::npos);
    CHECK(result.output.find("jobud.ready") == std::string::npos);
    CHECK(result.output.find("jobud.stopped") != std::string::npos);
    CHECK(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("native root helper verifies permanent drop and refuses retained capabilities",
          "[jobud][identity][native][root]")
{
    auto const target   = privileged_target();
    auto const retained = GENERATE(false, true);
    auto const result   = run_child(JOBUD_IDENTITY_TEST_HELPER, {retained ? "retain-caps" : "target", target});
    CHECK(result.exit.exit_code == 0);
    if (retained) {
        CHECK(result.output.find("jobud.privilege.drop_failed") != std::string::npos);
    }
    else {
        CHECK(result.output.find("verified uid=") != std::string::npos);
    }
    CHECK(::geteuid() == 0);
}

TEST_CASE("native root drop uses an explicitly requested primary group", "[jobud][identity][native][root]")
{
    auto const  target = privileged_target();
    auto const* group  = std::getenv("JOBU_TEST_PRIVILEGE_GROUP");
    if (group == nullptr) {
        SKIP("a provisioned target group is required for the primary-group override gate");
    }
    auto const result = run_child(JOBUD_IDENTITY_TEST_HELPER, {"target", target, group});
    CHECK(result.exit.exit_code == 0);
}

TEST_CASE("root daemon refusal occurs before database or endpoint creation regardless of CLI permission",
          "[jobud][identity][native][root]")
{
    if (::geteuid() != 0) {
        SKIP("native root daemon refusal requires a root test process");
    }
    jb::test::TemporaryDirectory directory;
    auto const                   database = directory.path() / "must-not-create.sqlite";
    auto const                   socket   = directory.path() / "must-not-create.sock";
    auto                         arguments =
        std::vector<std::string>{"--no-config", "--database", database.string(), "--socket", socket.string()};
    if (GENERATE(false, true)) {
        arguments.emplace_back("--allow-root-cli");
    }
    auto const result = run_child(JOBUD_EXECUTABLE, std::move(arguments));
    CHECK(result.exit.exit_code == 1);
    CHECK(result.output.find("jobud.privilege.root_disallowed") != std::string::npos);
    CHECK(result.output.find("jobud.ready") == std::string::npos);
    CHECK(result.output.find("jobud.stopped") != std::string::npos);
    CHECK(result.output.find("jobud.unsafe.root_cli") == std::string::npos);
    CHECK(result.output.find("jobud.unsafe.root_daemon") == std::string::npos);
    CHECK(std::filesystem::is_empty(directory.path()));
}

TEST_CASE("root daemon constructs resources under the requested final user even with both unsafe flags",
          "[jobud][identity][native][root]")
{
    auto const target     = privileged_target();
    auto       operations = jb::jobud::detail::make_system_privilege_operations();
    auto       account    = operations->account(target);
    REQUIRE(account);

    // The fixture provisions its own leaf. Production directory preparation belongs to Stage 9.18.
    jb::test::TemporaryDirectory directory;
    REQUIRE(::chown(directory.path().c_str(), account->user, account->primary_group) == 0);
    auto const                 database = directory.path() / "daemon.sqlite";
    auto const                 socket   = directory.path() / "daemon.sock";
    Application                app{0, nullptr};
    Process                    daemon;
    std::string                log;
    bool                       ready{false};
    std::optional<ProcessExit> exit;
    daemon.standard_error.connect(&app, [&](ByteBuffer const& bytes) {
        log.append(as_string_view(bytes));
        if (!ready && log.find("jobud.ready") != std::string::npos) {
            ready = true;
            app.quit();
        }
    });
    daemon.finished.connect(&app, [&](ProcessExit const& value) {
        exit = value;
        app.quit();
    });
    REQUIRE(daemon.start({
        .executable = JOBUD_EXECUTABLE,
        .arguments  = {"--no-config",
                       "--database", database.string(),
                       "--socket", socket.string(),
                       "--run-as-user", target,
                       "--allow-root-daemon", "--allow-root-cli"},
        .timeout    = 10s
    }));
    static_cast<void>(app.exec());
    INFO(log);
    REQUIRE(ready);
    REQUIRE_FALSE(exit);
    CHECK(log.find("jobud.unsafe.root_daemon") == std::string::npos);
    CHECK(log.find("jobud.unsafe.root_cli") == std::string::npos);

    struct stat state{};
    REQUIRE(::stat(database.c_str(), &state) == 0);
    CHECK(state.st_uid == account->user);
    REQUIRE(::stat(socket.c_str(), &state) == 0);
    CHECK(state.st_uid == account->user);
    REQUIRE(daemon.stop());
    static_cast<void>(app.exec());
    REQUIRE(exit);
    CHECK(exit->exit_code == 0);
    CHECK(::geteuid() == 0);
}
