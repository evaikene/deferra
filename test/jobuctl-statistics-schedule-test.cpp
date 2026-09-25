#include "command_registry_priv.hpp"
#include "json.hpp"
#include "support/temporary_directory.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using jb::core::JsonValue;

class Daemon {
public:
    explicit Daemon(pid_t pid)
        : _pid{pid}
    {}

    ~Daemon()
    {
        if (_pid > 0) {
            static_cast<void>(::kill(_pid, SIGTERM));
            auto status = 0;
            while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    Daemon(Daemon const&)                    = delete;
    auto operator=(Daemon const&) -> Daemon& = delete;

    [[nodiscard]] auto alive() const -> bool { return ::kill(_pid, 0) == 0; }

private:
    pid_t _pid;
};

auto start_daemon(std::filesystem::path const& socket, std::filesystem::path const& database) -> std::unique_ptr<Daemon>
{
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execl(JOBUD_EXECUTABLE,
                JOBUD_EXECUTABLE,
                "--socket",
                socket.c_str(),
                "--database",
                database.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    auto daemon   = std::make_unique<Daemon>(pid);
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto error = std::error_code{};
        if (std::filesystem::is_socket(socket, error)) {
            return daemon;
        }
        REQUIRE(daemon->alive());
        std::this_thread::sleep_for(10ms);
    }
    FAIL("jobud did not create its socket before the deadline");
    return {};
}

auto read_all(int fd) -> std::string
{
    auto output = std::string{};
    auto block  = std::array<char, 4096>{};
    for (;;) {
        auto const count = ::read(fd, block.data(), block.size());
        if (count > 0) {
            output.append(block.data(), static_cast<std::size_t>(count));
        }
        else if (count == 0) {
            return output;
        }
        else if (errno != EINTR) {
            FAIL("unable to read child output");
            return {};
        }
    }
}

struct CommandResult {
    int         code{-1};
    std::string out;
    std::string err;
};

auto run_cli(std::filesystem::path const& socket, std::vector<std::string> arguments) -> CommandResult
{
    auto output_pipe = std::array<int, 2>{};
    auto error_pipe  = std::array<int, 2>{};
    REQUIRE(::pipe(output_pipe.data()) == 0);
    REQUIRE(::pipe(error_pipe.data()) == 0);

    arguments.insert(arguments.begin(), {JOBUCTL_EXECUTABLE, "--socket", socket.string()});
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 || ::dup2(error_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        for (auto fd : {output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]}) {
            ::close(fd);
        }
        ::alarm(10);
        ::execv(JOBUCTL_EXECUTABLE, argv.data());
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    // The exercised replies are bounded below the pipe capacity, so the child can exit before draining.
    auto status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    auto result = CommandResult{.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                                .out  = read_all(output_pipe[0]),
                                .err  = read_all(error_pipe[0])};
    ::close(output_pipe[0]);
    ::close(error_pipe[0]);
    return result;
}

auto json_reply(std::filesystem::path const& socket, std::vector<std::string> arguments) -> JsonValue
{
    arguments.emplace_back("--json");
    auto result = run_cli(socket, std::move(arguments));
    REQUIRE(result.code == 0);
    REQUIRE(result.err.empty());
    auto parsed = jb::core::parse_json(result.out);
    REQUIRE(parsed);
    return std::move(*parsed);
}

} // namespace

TEST_CASE("jobuctl exposes every advertised daemon method", "[jobuctl][integration]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   socket   = directory.path() / "jobud.sock";
    auto const                   database = directory.path() / "jobu.sqlite";
    auto                         daemon   = start_daemon(socket, database);

    auto info       = json_reply(socket, {"system", "info"});
    auto advertised = std::set<std::string>{};
    for (auto const& capability : info.as_object().at("capabilities").as_array()) {
        advertised.insert(capability.as_string());
    }
    CHECK(advertised.size() == info.as_object().at("capabilities").as_array().size());
    auto registered = std::set<std::string>{};
    for (auto const& command : jb::jobuctl::detail::command_specs()) {
        registered.emplace(command.capability);
    }
    CHECK(registered.size() == jb::jobuctl::detail::command_specs().size());
    CHECK(registered == advertised);
}

TEST_CASE("jobuctl statistics pages and cron previews use real daemon methods", "[jobuctl][integration]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   socket   = directory.path() / "jobud.sock";
    auto const                   database = directory.path() / "jobu.sqlite";
    auto                         daemon   = start_daemon(socket, database);

    auto        empty          = json_reply(socket, {"system", "stats"});
    auto const& empty_fields   = empty.as_object();
    auto const& default_window = empty_fields.at("window").as_object();
    auto        default_from   = jb::jobu::parse_utc_timestamp(default_window.at("from").as_string());
    auto        default_to     = jb::jobu::parse_utc_timestamp(default_window.at("to").as_string());
    REQUIRE(default_from);
    REQUIRE(default_to);
    CHECK(*default_to - *default_from == 24h);
    CHECK(empty_fields.at("group_by").as_string() == "none");
    CHECK(empty_fields.at("groups").as_array().size() == 1);
    CHECK(empty_fields.at("groups")
              .as_array()
              .front()
              .as_object()
              .at("schedule_lateness_ms")
              .as_object()
              .at("average")
              .is_null());
    CHECK(empty_fields.at("groups").as_array().front().as_object().at("runnable_wait_ms").is_null());
    CHECK(empty_fields.at("measurement").as_object().at("runnable_wait").as_string() == "unavailable");
    auto wide_window = run_cli(socket,
                               {"system",
                                "stats",
                                "--planned-from",
                                "2030-01-01T00:00:00Z",
                                "--planned-to",
                                "2030-02-02T00:00:00Z",
                                "--json"});
    CHECK(wide_window.code == 1);
    CHECK(wide_window.out.empty());

    for (auto const& name : {"reports", "alerts"}) {
        CHECK(run_cli(socket, {"queue", "create", name}).code == 0);
        CHECK(run_cli(socket,
                      {"job",
                       "create",
                       "--queue-name",
                       name,
                       "--type",
                       "cli",
                       "--at",
                       "2100-01-01T00:00:00Z",
                       "--command",
                       "/bin/true"})
                  .code == 0);
    }
    CHECK(run_cli(socket,
                  {"job",
                   "create",
                   "--queue-name",
                   "reports",
                   "--type",
                   "cli",
                   "--at",
                   "2100-01-01T00:00:00Z",
                   "--command",
                   "/bin/true"})
              .code == 0);

    auto        initial = json_reply(socket,
                                     {"system",
                                      "stats",
                                      "--planned-from",
                                      "2099-12-31T00:00:00Z",
                                      "--planned-to",
                                      "2100-01-02T00:00:00Z",
                                      "--group-by",
                                      "queue",
                                      "--limit",
                                      "1"});
    auto const& first   = initial.as_object();
    REQUIRE(first.at("groups").as_array().size() == 1);
    auto const first_count =
        first.at("groups").as_array().front().as_object().at("runs").as_object().at("total").as_uint();
    REQUIRE(first.at("next_cursor").is_string());
    auto cursor = first.at("next_cursor").as_string();

    auto        continued = json_reply(socket, {"system", "stats", "--cursor", cursor});
    auto const& second    = continued.as_object();
    REQUIRE(second.at("groups").as_array().size() == 1);
    CHECK(second.at("next_cursor").is_null());
    CHECK(second.at("window") == first.at("window"));
    CHECK(second.at("groups").as_array().front().as_object().at("key") !=
          first.at("groups").as_array().front().as_object().at("key"));
    CHECK(first_count +
              second.at("groups").as_array().front().as_object().at("runs").as_object().at("total").as_uint() ==
          3);

    auto queue = json_reply(socket,
                            {"queue",
                             "stats",
                             "--name",
                             "reports",
                             "--planned-from",
                             "2099-12-31T00:00:00Z",
                             "--planned-to",
                             "2100-01-02T00:00:00Z"});
    CHECK(queue.as_object().at("groups").as_array().front().as_object().at("runs").as_object().at("total").as_uint() ==
          2);
    auto scoped_first = json_reply(socket,
                                   {"queue",
                                    "stats",
                                    "--name",
                                    "reports",
                                    "--planned-from",
                                    "2099-12-31T00:00:00Z",
                                    "--planned-to",
                                    "2100-01-02T00:00:00Z",
                                    "--group-by",
                                    "job",
                                    "--limit",
                                    "1"});
    REQUIRE(scoped_first.as_object().at("next_cursor").is_string());
    auto scoped_cursor = scoped_first.as_object().at("next_cursor").as_string();
    auto scoped_second = json_reply(socket, {"queue", "stats", "--cursor", scoped_cursor});
    CHECK(scoped_second.as_object().at("groups").as_array().size() == 1);
    CHECK(scoped_second.as_object().at("next_cursor").is_null());
    auto wrong_method = run_cli(socket, {"system", "stats", "--cursor", scoped_cursor, "--json"});
    CHECK(wrong_method.code == 1);
    CHECK(wrong_method.out.empty());

    auto human = run_cli(socket, {"system", "stats"});
    CHECK(human.code == 0);
    CHECK(human.out.find("Runnable wait (ms): unavailable") != std::string::npos);
    CHECK(human.out.find("average=null") != std::string::npos);

    auto valid = json_reply(socket, {"schedule", "validate", "0 9 * * FRI-MON"});
    CHECK(valid.as_object().at("valid").as_bool());
    auto human_valid = run_cli(socket, {"schedule", "validate", "@daily"});
    CHECK(human_valid.code == 0);
    CHECK(human_valid.out == "Schedule is valid\n");
    auto utc = json_reply(socket, {"schedule", "next", "@daily", "--after", "2030-01-01T00:00:00Z", "--count", "1"});
    CHECK(utc.as_object().at("occurrences").as_array().front().as_string() == "2030-01-02T00:00:00.000000Z");
    auto human_next =
        run_cli(socket, {"schedule", "next", "@daily", "--after", "2030-01-01T00:00:00Z", "--count", "1"});
    CHECK(human_next.code == 0);
    CHECK(human_next.out == "2030-01-02T00:00:00.000000Z\n");
    auto local = json_reply(socket,
                            {"schedule",
                             "next",
                             "@daily",
                             "--timezone",
                             "Europe/Tallinn",
                             "--after",
                             "2030-01-01T00:00:00Z",
                             "--count",
                             "1"});
    CHECK(local.as_object().at("occurrences").as_array().front().as_string() == "2030-01-01T22:00:00.000000Z");

    auto invalid = run_cli(socket, {"schedule", "validate", "not-a-cron", "--json"});
    CHECK(invalid.code == 1);
    CHECK(invalid.out.empty());
    CHECK(invalid.err.find("\"kind\":\"remote\"") != std::string::npos);
}
