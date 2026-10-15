#include "application.hpp"
#include "byte_buffer.hpp"
#include "database.hpp"
#include "json.hpp"
#include "privileges_priv.hpp"
#include "process.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "support/temporary_directory.hpp"
#include "text_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <memory>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

/// The process owns all callbacks borrowing this fixture; it is destroyed before their captures.
struct Daemon {
    explicit Daemon(Application& application)
        : app{application}
    {}

    void launch(std::filesystem::path const&         database,
                std::filesystem::path const&         socket,
                std::optional<std::filesystem::path> config = std::nullopt)
    {
        process = std::make_unique<Process>();
        process->standard_error.connect(&app, [&](ByteBuffer const& bytes) { log.append(as_string_view(bytes)); });
        process->finished.connect(&app, [&](ProcessExit const& value) { exit = value; });
        auto arguments =
            std::vector<std::string>{"--no-config", "--database", database.string(), "--socket", socket.string()};
        if (config) {
            arguments[0] = "--config";
            arguments.insert(arguments.begin() + 1, config->string());
        }
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-daemon");
        }
        REQUIRE(process->start({.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments), .timeout = 15s}));
    }

    void ready()
    {
        until(app, [&] { return exit || log.find("jobud.ready") != std::string::npos; });
        INFO(log);
        REQUIRE_FALSE(exit);
    }

    void failed(std::string const& code)
    {
        until(app, [&] { return exit.has_value(); });
        INFO(log);
        REQUIRE(exit->kind == ProcessExitKind::Exited);
        CHECK(exit->exit_code == 1);
        CHECK(log.find(code) != std::string::npos);
        CHECK(log.find("jobud.ready") == std::string::npos);
        CHECK(log.find("jobud.recovery.completed") == std::string::npos);
    }

    void stop(int signal = SIGTERM)
    {
        REQUIRE(process->process_id());
        REQUIRE(::kill(static_cast<pid_t>(*process->process_id()), signal) == 0);
        until(app, [&] { return exit.has_value(); });
        if (signal == SIGTERM) {
            CHECK(exit->kind == ProcessExitKind::Exited);
            CHECK(exit->exit_code == 0);
        }
        else {
            CHECK(exit->kind == ProcessExitKind::Signaled);
            CHECK(exit->signal_number == signal);
        }
        process.reset();
    }

    Application&               app;
    std::string                log;
    std::optional<ProcessExit> exit;
    std::unique_ptr<Process>   process;
};

auto metadata(std::filesystem::path const& path) -> struct stat {
    struct stat value{};
    REQUIRE(::lstat(path.c_str(), &value) == 0);
    return value;

}

auto failure_fields(std::string const& output, std::string_view format) -> JsonValue::Object

{
    auto const event = output.find("jobud.failed");
    REQUIRE(event != std::string::npos);
    auto const previous = output.rfind('\n', event);
    auto const begin    = previous == std::string::npos ? 0U : previous + 1U;
    auto const end      = output.find('\n', event);
    REQUIRE(end != std::string::npos);
    auto line = std::string_view{output}.substr(begin, end - begin);
    if (format == "text") {
        auto const fields = line.find(" fields=");
        REQUIRE(fields != std::string_view::npos);
        line.remove_prefix(fields + 8U);
    }
    auto parsed = parse_json(line);
    REQUIRE(parsed);
    return format == "text" ? parsed->as_object() : parsed->as_object().at("fields").as_object();
}

auto provisioned(char const* variable) -> std::string

{
    auto const* value = std::getenv(variable);
    if (::geteuid() != 0 || value == nullptr) {
        SKIP("socket administrative-group access requires root and explicitly provisioned disposable identities");
    }
    return value;
}

auto probe_as(Application& app, std::string const& account, std::filesystem::path const& socket) -> int
{
    Process                    process;
    std::optional<ProcessExit> exit;
    process.finished.connect(&app, [&](ProcessExit const& value) { exit = value; });
    REQUIRE(process.start({
        .executable = JOBUD_ENDPOINT_TEST_HELPER,
        .arguments  = {account, socket.string()},
        .timeout    = 5s
    }));
    until(app, [&] { return exit.has_value(); });
    REQUIRE(exit->kind == ProcessExitKind::Exited);
    REQUIRE(exit->exit_code);
    return *exit->exit_code;
}

} // namespace

TEST_CASE("Daemon endpoint competition rejects another database before creation", "[jobud][endpoint][integration]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   socket = directory.path() / "daemon.sock";
    Daemon                       first{app};
    first.launch(directory.path() / "first.sqlite", socket);
    first.ready();
    auto const socket_inode = metadata(socket).st_ino;
    auto const lock_inode   = metadata(socket.string() + ".lock").st_ino;

    Daemon     second{app};
    auto const second_database = directory.path() / "second.sqlite";
    second.launch(second_database, socket);
    second.failed("jobud.socket.in_use");
    CHECK_FALSE(std::filesystem::exists(second_database));
    CHECK(metadata(socket).st_ino == socket_inode);
    CHECK(metadata(socket.string() + ".lock").st_ino == lock_inode);
    first.stop();
    CHECK_FALSE(std::filesystem::exists(socket));
    CHECK(metadata(socket.string() + ".lock").st_ino == lock_inode);
}

TEST_CASE("Daemon restarts the same endpoint after clean stop or SIGKILL", "[jobud][endpoint][integration]")
{
    auto const                   signal = GENERATE(SIGTERM, SIGKILL);
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   socket   = directory.path() / "daemon.sock";
    auto const                   database = directory.path() / "daemon.sqlite";
    Daemon                       first{app};
    first.launch(database, socket);
    first.ready();
    auto const lock_inode = metadata(socket.string() + ".lock").st_ino;
    first.stop(signal);
    CHECK(std::filesystem::exists(socket) == (signal == SIGKILL));
    Daemon next{app};
    next.launch(database, socket);
    next.ready();
    CHECK(metadata(socket.string() + ".lock").st_ino == lock_inode);
    next.stop();
    CHECK_FALSE(std::filesystem::exists(socket));
}

TEST_CASE("Database ownership failure preserves the crashed socket", "[jobud][endpoint][integration]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   socket   = directory.path() / "daemon.sock";
    auto const                   database = directory.path() / "daemon.sqlite";
    Daemon                       first{app};
    first.launch(database, socket);
    first.ready();
    first.stop(SIGKILL);
    auto const       before = metadata(socket);
    jb::db::Database owner{
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = database})};
    REQUIRE(owner.open());
    Daemon rejected{app};
    rejected.launch(database, socket);
    rejected.failed("db.sqlite.already_in_use");
    CHECK(metadata(socket).st_ino == before.st_ino);
    REQUIRE(owner.close());
    Daemon next{app};
    next.launch(database, socket);
    next.ready();
    next.stop();
}

TEST_CASE("Daemon endpoint collisions preserve non-socket entries before recovery", "[jobud][endpoint][integration]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   socket = directory.path() / "daemon.sock";
    SECTION("regular file")
    {
        std::ofstream{socket} << "preserve";
    }
    SECTION("symlink")
    {
        std::filesystem::create_symlink("missing", socket);
    }
    auto const before = metadata(socket);
    Daemon     rejected{app};
    rejected.launch(directory.path() / "daemon.sqlite", socket);
    rejected.failed("jobud.path.unsafe");
    CHECK(metadata(socket).st_ino == before.st_ino);
    CHECK(metadata(socket).st_mode == before.st_mode);
}

TEST_CASE("Daemon startup explains missing ancestors and wrong directory modes", "[jobud][paths][integration]")
{
    auto const*                  format = GENERATE("json", "text");
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   config = directory.path() / "daemon.ini";
    std::ofstream{config} << "logging.format = " << format << '\n';
    REQUIRE(::chmod(config.c_str(), 0600) == 0);
    auto                  database = directory.path() / "state/database.sqlite";
    auto                  socket   = directory.path() / "runtime/daemon.sock";
    std::filesystem::path failing;
    std::string           resource;
    std::string           reason;
    SECTION("missing state ancestor")
    {
        failing  = directory.path() / "missing var";
        database = failing / "lib/database.sqlite";
        resource = "state_directory";
        reason   = "missing_parent";
    }
    SECTION("wrong state mode")
    {
        failing = database.parent_path();
        REQUIRE(std::filesystem::create_directory(failing));
        REQUIRE(::chmod(failing.c_str(), 0755) == 0);
        resource = "state_directory";
        reason   = "leaf_owner_or_mode";
    }
    SECTION("wrong runtime mode")
    {
        failing = socket.parent_path();
        REQUIRE(std::filesystem::create_directory(failing));
        REQUIRE(::chmod(failing.c_str(), 0755) == 0);
        resource = "runtime_directory";
        reason   = "leaf_owner_or_mode";
    }
    Daemon daemon{app};
    daemon.launch(database, socket, config);
    daemon.failed(reason == "missing_parent" ? "jobud.path.inspect_failed" : "jobud.path.unsafe");
    auto const fields = failure_fields(daemon.log, format);
    CHECK(fields.at("reason").as_string() == reason);
    CHECK(fields.at("path").as_string() == failing.string());
    CHECK(fields.at("resource").as_string() == resource);
    CHECK_FALSE(fields.at("action").as_string().empty());
    CHECK_FALSE(std::filesystem::exists(database));
    CHECK_FALSE(std::filesystem::exists(socket));
    if (reason == "missing_parent") {
        CHECK(daemon.log.find("is missing") != std::string::npos);
        CHECK(daemon.log.find("Provision") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(failing));
    }
    else {
        CHECK(fields.at("actual_mode").as_string() == "0755");
        CHECK(fields.at("required_mode").as_string() == "0700");
        CHECK(daemon.log.find("required permissions are 0700") != std::string::npos);
        CHECK((metadata(failing).st_mode & 07777) == 0755);
    }
}

TEST_CASE("Configuration path failures are actionable before logger installation",
          "[jobud][paths][config][integration]")
{
    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto                         config = directory.path() / "missing parent/daemon.ini";
    SECTION("explicit missing configuration parent")
    {}
    SECTION("wrong configuration permissions do not disclose contents")
    {
        config = directory.path() / "daemon.ini";
        std::ofstream{config} << "http.proxy = http://stage936-private-credential@proxy.test\n";
        REQUIRE(::chmod(config.c_str(), 0620) == 0);
    }
    SECTION("alias target containing controls and malformed UTF-8")
    {
        auto const hostile = directory.path() / std::string{"a 'quoted'\n\x1b\xff"};
        REQUIRE(std::filesystem::create_directory(hostile));
        REQUIRE(::chmod(hostile.c_str(), 0777) == 0);
        std::filesystem::create_directory_symlink(hostile, directory.path() / "alias");
        config = directory.path() / "alias/daemon.ini";
    }
    Daemon daemon{app};
    daemon.launch(directory.path() / "database.sqlite", directory.path() / "daemon.sock", config);
    daemon.failed("jobud.config.read_failed");
    CHECK(daemon.log.find("Action:") != std::string::npos);
    CHECK((daemon.log.find("Configured resource:") != std::string::npos ||
           daemon.log.find(config.string()) != std::string::npos));
    CHECK(daemon.log.find("jobud.starting") == std::string::npos);
    CHECK(daemon.log.find("stage936-private-credential") == std::string::npos);
    CHECK(daemon.log.find('\x1b') == std::string::npos);
    CHECK(jb::core::is_valid_utf8(daemon.log));
    CHECK(std::count(daemon.log.begin(), daemon.log.end(), '\n') == 1);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "database.sqlite"));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "daemon.sock"));
}

TEST_CASE("Configured socket group admits administrators and rejects outsiders", "[jobud][endpoint][integration][root]")
{
    auto const target        = provisioned("JOBU_TEST_PRIVILEGE_USER");
    auto const admin         = provisioned("JOBU_TEST_SOCKET_ADMIN_USER");
    auto const outsider      = provisioned("JOBU_TEST_SOCKET_OUTSIDER_USER");
    auto const supplementary = GENERATE(false, true);
    auto const mode          = GENERATE(0600, 0660);
    auto       operations    = jb::jobud::detail::make_system_privilege_operations();
    auto       account       = operations->account(target);
    REQUIRE(account);
    REQUIRE(account->user != 0);
    auto const* primary = ::getgrgid(account->primary_group);
    REQUIRE(primary);
    auto group_name = std::string{primary->gr_name};
    if (supplementary) {
        group_name = provisioned("JOBU_TEST_PRIVILEGE_GROUP");
    }
    auto group = operations->group(group_name);
    REQUIRE(group);

    Application                  app{0, nullptr};
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                           std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                           std::filesystem::perms::others_exec};
    auto const                   config = directory.path() / "daemon.ini";
    std::ofstream{config} << "daemon.run_as_user = " << target << "\nsocket.group = " << group_name
                          << "\nsocket.mode = " << (mode == 0660 ? "0660" : "0600") << '\n';
    REQUIRE(::chmod(config.c_str(), 0600) == 0);
    auto const socket = directory.path() / "runtime" / "daemon.sock";
    Daemon     daemon{app};
    daemon.launch(directory.path() / "state" / "daemon.sqlite", socket, config);
    daemon.ready();
    auto const bound = metadata(socket);
    CHECK(bound.st_uid == account->user);
    CHECK(bound.st_gid == *group);
    CHECK((bound.st_mode & 07777) == mode);
    auto const runtime = metadata(socket.parent_path());
    CHECK(runtime.st_gid == *group);
    CHECK((runtime.st_mode & 07777) == (mode == 0660 ? 0750 : 0700));
    auto const lock = metadata(socket.string() + ".lock");
    CHECK(lock.st_uid == account->user);
    CHECK((lock.st_mode & 07777) == 0600);
    CHECK(probe_as(app, target, socket) == 0);
    CHECK(probe_as(app, admin, socket) == (mode == 0660 ? 0 : 1));
    CHECK(probe_as(app, outsider, socket) == 1);
    daemon.stop();
}
