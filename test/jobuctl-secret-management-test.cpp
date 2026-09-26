#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/temporary_directory.hpp"

#include "byte_buffer.hpp"
#include "database.hpp"
#include "json.hpp"
#include "secret_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class Daemon {
public:
    explicit Daemon(pid_t pid)
        : _pid{pid}
    {}

    ~Daemon() { stop(); }

    void stop()
    {
        if (_pid > 0) {
            static_cast<void>(::kill(_pid, SIGTERM));
            auto status = 0;
            while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
            }
            _pid = -1;
        }
    }

    Daemon(Daemon const&)                    = delete;
    auto operator=(Daemon const&) -> Daemon& = delete;

    [[nodiscard]] auto alive() const -> bool { return ::kill(_pid, 0) == 0; }

private:
    pid_t _pid;
};

auto start_daemon(std::filesystem::path const& executable,
                  std::filesystem::path const& socket,
                  std::filesystem::path const& database) -> std::unique_ptr<Daemon>
{
    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execl(executable.c_str(),
                executable.c_str(),
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
        std::error_code error;
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

#if defined(__linux__)
void check_process_arguments(pid_t pid, std::filesystem::path const& executable, std::string_view secret)
{
    // Keep stdin open until exec has happened, so /proc exposes the actual jobuctl argument vector.
    auto const prefix   = executable.string() + '\0';
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto const path = std::filesystem::path{"/proc"} / std::to_string(pid) / "cmdline";
        auto       file = std::ifstream{path, std::ios::binary};
        auto       args = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        if (args.starts_with(prefix)) {
            CHECK(args.find(secret) == std::string::npos);
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL("unable to inspect jobuctl process arguments");
}
#endif

struct CommandResult {
    int         code{-1};
    std::string out;
    std::string err;
};

auto run_cli(std::filesystem::path const& executable,
             std::filesystem::path const& socket,
             std::vector<std::string>     arguments,
             std::string_view             input             = {},
             bool                         inspect_arguments = false) -> CommandResult
{
    auto input_pipe  = std::array<int, 2>{};
    auto output_pipe = std::array<int, 2>{};
    auto error_pipe  = std::array<int, 2>{};
    REQUIRE(::pipe(input_pipe.data()) == 0);
    REQUIRE(::pipe(output_pipe.data()) == 0);
    REQUIRE(::pipe(error_pipe.data()) == 0);

    arguments.insert(arguments.begin(), {executable.string(), "--socket", socket.string()});
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    auto const pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        if (::dup2(input_pipe[0], STDIN_FILENO) < 0 || ::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(error_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        for (auto fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]}) {
            ::close(fd);
        }
        ::alarm(10);
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
#if defined(__linux__)
    if (inspect_arguments) {
        check_process_arguments(pid, executable, input.substr(0, input.find('\n')));
    }
#else
    static_cast<void>(inspect_arguments);
#endif
    auto written = std::size_t{0};
    while (written < input.size()) {
        auto const count = ::write(input_pipe[1], input.data() + written, input.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        written += static_cast<std::size_t>(count);
    }
    ::close(input_pipe[1]);

    // These commands produce bounded metadata or short errors, so their pipe output fits before waitpid.
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

auto stored_value(std::filesystem::path const& database_path, std::string_view name) -> jb::core::ByteBuffer
{
    auto database = jb::db::Database{
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = database_path})};
    REQUIRE(database.open());
    auto repository = jb::jobu::detail::SecretRepository{database};
    auto value      = repository.find_value(name);
    REQUIRE(value);
    REQUIRE(database.close());
    return std::move(value).value();
}

auto write_file(std::filesystem::path const& path, std::string_view bytes) -> void
{
    auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
    REQUIRE(file);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("jobuctl secret commands preserve bytes and expose only metadata", "[jobuctl][secret]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   socket   = directory.path() / "jobud.sock";
    auto const                   database = directory.path() / "jobu.sqlite";
    auto const                   file     = directory.path() / "secret.bin";
    auto const                   request  = directory.path() / "request.json";
    auto                         daemon   = start_daemon(JOBUD_EXECUTABLE, socket, database);

    auto const binary = std::string{"\0\xff\n", 3};
    write_file(file, binary);
    auto set =
        run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "set", "reports.token", "--file", file.string(), "--json"});
    REQUIRE(set.code == 0);
    CHECK(set.err.empty());
    auto first = jb::core::parse_json(set.out);
    REQUIRE(first);
    CHECK(first->as_object().at("name").as_string() == "reports.token");
    CHECK_FALSE(first->as_object().contains("value"));
    auto binary_set = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "set", "reports.binary", "--file", file.string()});
    REQUIRE(binary_set.code == 0);

    auto const marker        = std::string{"rotated-secret-sentinel"};
    auto const rotated_bytes = marker + '\n';
    auto       rotated       = run_cli(JOBUCTL_EXECUTABLE,
                                       socket,
                                       {"secret", "set", "reports.token", "--stdin", "--json"},
                                       rotated_bytes,
                                       true);
    REQUIRE(rotated.code == 0);
    CHECK(rotated.out.find(marker) == std::string::npos);
    CHECK(rotated.err.find(marker) == std::string::npos);
    auto second = jb::core::parse_json(rotated.out);
    REQUIRE(second);
    CHECK(second->as_object().at("created_at") == first->as_object().at("created_at"));

    write_file(file, {});
    auto empty = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "set", "reports.empty", "--file", file.string()});
    REQUIRE(empty.code == 0);

    auto page = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "list", "--limit", "1", "--json"});
    REQUIRE(page.code == 0);
    auto first_page = jb::core::parse_json(page.out);
    REQUIRE(first_page);
    auto const& first_items = first_page->as_object().at("items").as_array();
    REQUIRE(first_items.size() == 1);
    CHECK(first_items.front().as_object().size() == 3);
    CHECK(first_items.front().as_object().at("name").as_string() == "reports.binary");
    auto const after = first_page->as_object().at("next_after_name").as_string();
    CHECK(after == "reports.binary");
    auto continuation =
        run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "list", "--after-name", after, "--limit", "1", "--json"});
    REQUIRE(continuation.code == 0);
    CHECK(continuation.out.find(marker) == std::string::npos);
    auto second_page = jb::core::parse_json(continuation.out);
    REQUIRE(second_page);
    CHECK(second_page->as_object().at("items").as_array().front().as_object().at("name").as_string() ==
          "reports.empty");

    write_file(request, R"({"name":"reports.unused","value":{"encoding":"base64","data":""}})");
    auto unused = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "set", "--request-file", request.string(), "--json"});
    REQUIRE(unused.code == 0);

    write_file(
        request,
        R"({"queue_name":"reports","type":"cli","schedule":{"kind":"once","at":"2100-01-01T00:00:00Z"},"payload":{"command":"/bin/true","arguments":[{"secret":"reports.token"}]}})");
    auto queue = run_cli(JOBUCTL_EXECUTABLE, socket, {"queue", "create", "reports", "--json"});
    REQUIRE(queue.code == 0);
    auto job = run_cli(JOBUCTL_EXECUTABLE, socket, {"job", "create", "--request-file", request.string(), "--json"});
    REQUIRE(job.code == 0);

    auto conflict = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "delete", "reports.token", "--json"});
    REQUIRE(conflict.code == 1);
    CHECK(conflict.out.empty());
    CHECK(conflict.err.find("jobu.secret.in_use") != std::string::npos);
    CHECK(conflict.err.find(marker) == std::string::npos);

    auto erased = run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "delete", "reports.unused", "--json"});
    REQUIRE(erased.code == 0);
    CHECK(erased.out == "null\n");
    CHECK(erased.err.empty());

    write_file(file, std::string(65537, 'x'));
    auto oversized =
        run_cli(JOBUCTL_EXECUTABLE, socket, {"secret", "set", "reports.large", "--file", file.string(), "--json"});
    REQUIRE(oversized.code == 2);
    CHECK(oversized.out.empty());
    CHECK(oversized.err.find("jobuctl.input.too_large") != std::string::npos);

    // The daemon owns the SQLite database exclusively while serving. Inspect committed bytes after shutdown.
    daemon->stop();
    CHECK(jb::core::as_string_view(stored_value(database, "reports.binary")) == binary);
    CHECK(jb::core::as_string_view(stored_value(database, "reports.token")) == rotated_bytes);
    CHECK(stored_value(database, "reports.empty").empty());
}
