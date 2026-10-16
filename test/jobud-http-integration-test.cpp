#include "support/http_test_server.hpp"
#include "support/temporary_directory.hpp"

#include "byte_buffer.hpp"
#include "json.hpp"

#include <fmt/format.h>

#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef __APPLE__
#  include <crt_externs.h>
#endif
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

auto process_environment() noexcept -> char**
{
#ifdef __APPLE__
    // macOS exposes the process environment through this accessor instead of declaring environ in unistd.h.
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

class ChildProcess {
public:
    explicit ChildProcess(pid_t pid)
        : _pid{pid}
    {}

    ~ChildProcess() { terminate(); }

    ChildProcess(ChildProcess const&)                    = delete;
    auto operator=(ChildProcess const&) -> ChildProcess& = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : _pid{std::exchange(other._pid, -1)}
        , _status{std::exchange(other._status, std::nullopt)}
    {}

    auto operator=(ChildProcess&& other) noexcept -> ChildProcess&
    {
        if (this != &other) {
            terminate();
            _pid    = std::exchange(other._pid, -1);
            _status = std::exchange(other._status, std::nullopt);
        }
        return *this;
    }

    [[nodiscard]] auto try_wait() -> std::optional<int>
    {
        if (_status || _pid < 0) {
            return _status;
        }

        auto status = 0;
        auto result = pid_t{};
        do {
            result = ::waitpid(_pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);

        if (result == _pid) {
            _pid    = -1;
            _status = status;
        }
        return _status;
    }

    void terminate() noexcept
    {
        if (_pid < 0) {
            return;
        }

        static_cast<void>(::kill(_pid, SIGTERM));
        auto const deadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            if (try_wait()) {
                return;
            }
            std::this_thread::sleep_for(10ms);
        }

        static_cast<void>(::kill(_pid, SIGKILL));
        auto status = 0;
        while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
        }
        _pid    = -1;
        _status = status;
    }

private:
    pid_t              _pid{-1};
    std::optional<int> _status;
};

struct CommandResult {
    bool        completed{false};
    int         status{0};
    std::string output;
    std::string error_output;
};

// Own each capture pipe across every spawn/setup failure; only its reader remains open in the parent after spawn.
struct CommandPipe {
    CommandPipe()                                      = default;
    CommandPipe(CommandPipe const&)                    = delete;
    auto operator=(CommandPipe const&) -> CommandPipe& = delete;

    ~CommandPipe()
    {
        for (auto fd : descriptors) {
            if (fd >= 0) {
                static_cast<void>(::close(fd));
            }
        }
    }

    auto open() -> bool
    {
        if (::pipe(descriptors) < 0) {
            return false;
        }
        auto const flags = ::fcntl(descriptors[0], F_GETFL, 0);
        return flags >= 0 && ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void close_writer() { static_cast<void>(::close(std::exchange(descriptors[1], -1))); }

    int descriptors[2]{-1, -1};
};

auto fail(std::string_view message) -> int
{
    fmt::print(stderr, "jobud-http-integration-test: {}\n", message);
    return EXIT_FAILURE;
}

void read_available(int fd, std::string& output, bool& eof)
{
    char buffer[4096];
    while (!eof) {
        auto const bytes = ::read(fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            output.append(buffer, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes == 0) {
            eof = true;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        eof = true;
    }
}

auto run_command(std::filesystem::path const& executable, std::vector<std::string> arguments) -> CommandResult
{
    CommandPipe output_pipe;
    CommandPipe error_pipe;
    if (!output_pipe.open() || !error_pipe.open()) {
        return {};
    }

    arguments.insert(arguments.begin(), executable.string());
    auto exec_arguments = std::vector<char*>{};
    exec_arguments.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) {
        exec_arguments.push_back(argument.data());
    }
    exec_arguments.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        return {};
    }
    auto const actions_ready =
        ::posix_spawn_file_actions_addclose(&actions, output_pipe.descriptors[0]) == 0 &&
        ::posix_spawn_file_actions_addclose(&actions, error_pipe.descriptors[0]) == 0 &&
        ::posix_spawn_file_actions_adddup2(&actions, output_pipe.descriptors[1], STDOUT_FILENO) == 0 &&
        ::posix_spawn_file_actions_adddup2(&actions, error_pipe.descriptors[1], STDERR_FILENO) == 0 &&
        ::posix_spawn_file_actions_addclose(&actions, output_pipe.descriptors[1]) == 0 &&
        ::posix_spawn_file_actions_addclose(&actions, error_pipe.descriptors[1]) == 0;
    auto pid = pid_t{};
    auto spawned =
        actions_ready
            ? ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, exec_arguments.data(), process_environment())
            : EINVAL;
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    if (spawned != 0) {
        return {};
    }

    output_pipe.close_writer();
    error_pipe.close_writer();

    ChildProcess  command{pid};
    CommandResult result;
    auto          eof       = false;
    auto          error_eof = false;
    auto const    deadline  = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        read_available(output_pipe.descriptors[0], result.output, eof);
        read_available(error_pipe.descriptors[0], result.error_output, error_eof);
        auto const status = command.try_wait();
        if (status && eof && error_eof) {
            result.completed = true;
            result.status    = *status;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    return result;
}

[[nodiscard]] auto successful(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_SUCCESS;
}

[[nodiscard]] auto failed(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) != EXIT_SUCCESS;
}

auto run_jobuctl(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> CommandResult
{
    auto owned_arguments = std::vector<std::string>{"--socket", socket_path.string()};
    owned_arguments.reserve(owned_arguments.size() + arguments.size());
    for (auto const argument : arguments) {
        owned_arguments.emplace_back(argument);
    }
    return run_command(executable, std::move(owned_arguments));
}

auto run_success(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> std::optional<std::string>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!successful(result)) {
        fmt::print(stderr, "{}{}", result.output, result.error_output);
        return std::nullopt;
    }
    return std::move(result.output);
}

auto spawn_jobud(std::filesystem::path const& executable,
                 std::filesystem::path const& socket_path,
                 std::filesystem::path const& database_path,
                 std::uint32_t                http_concurrency) -> std::optional<ChildProcess>
{
    auto arguments = std::vector<std::string>{
        executable.string(),
        "--no-config",
        "--socket",
        socket_path.string(),
        "--database",
        database_path.string(),
        "--http-concurrency",
        fmt::format("{}", http_concurrency),
    };
    if (::geteuid() == 0) {
        arguments.emplace_back("--allow-root-daemon");
    }
    auto exec_arguments = std::vector<char*>{};
    exec_arguments.reserve(arguments.size() + 1U);
    for (auto& argument : arguments) {
        exec_arguments.push_back(argument.data());
    }
    exec_arguments.push_back(nullptr);

    auto pid = pid_t{};
    auto spawned =
        ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, exec_arguments.data(), process_environment());
    if (spawned != 0) {
        return std::nullopt;
    }
    return ChildProcess{pid};
}

auto wait_for_listener(ChildProcess& daemon, std::filesystem::path const& socket_path) -> bool
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_socket(socket_path, error)) {
            return true;
        }
        if (daemon.try_wait()) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

auto rejects_usage(std::filesystem::path const& jobud,
                   std::filesystem::path const& socket_path,
                   std::filesystem::path const& database_path,
                   std::vector<std::string>     extra_arguments) -> bool
{
    auto arguments =
        std::vector<std::string>{"--no-config", "--socket", socket_path.string(), "--database", database_path.string()};
    arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());
    auto result = run_command(jobud, std::move(arguments));
    return failed(result) && result.error_output.find("jobud.startup.invalid_arguments") != std::string::npos &&
           !std::filesystem::exists(socket_path) && !std::filesystem::exists(database_path);
}

auto rejects_client_options(std::filesystem::path const& jobud,
                            std::filesystem::path const& socket_path,
                            std::filesystem::path const& database_path,
                            std::string_view             option,
                            std::string_view             value) -> bool
{
    auto result = run_command(jobud,
                              {
                                  "--no-config",
                                  "--socket",
                                  socket_path.string(),
                                  "--database",
                                  database_path.string(),
                                  std::string{option},
                                  std::string{value},
                              });
    return failed(result) && result.error_output.find("jobud.config.invalid") != std::string::npos &&
           !std::filesystem::exists(socket_path);
}

enum class DurableProbeResult : std::uint8_t {
    Waiting,
    Complete,
    Invalid,
};

auto column_text(sqlite3_stmt* statement, int column) -> std::string_view
{
    auto const* text = sqlite3_column_text(statement, column);
    auto const  size = sqlite3_column_bytes(statement, column);
    return text && size >= 0 ? std::string_view{reinterpret_cast<char const*>(text), static_cast<std::size_t>(size)}
                             : std::string_view{};
}

auto column_blob(sqlite3_stmt* statement, int column) -> std::string_view
{
    auto const* bytes = sqlite3_column_blob(statement, column);
    auto const  size  = sqlite3_column_bytes(statement, column);
    return bytes && size >= 0 ? std::string_view{static_cast<char const*>(bytes), static_cast<std::size_t>(size)}
                              : std::string_view{};
}

auto probe_durable_completion(sqlite3* database, std::string& reason) -> DurableProbeResult
{
    constexpr auto sql =
        "SELECT runs.state, attempts.state, attempts.outcome, attempts.result_json, output.stdout_blob, "
        "output.stderr_blob, output.stdout_truncated, output.stderr_truncated, output.capture_lost "
        "FROM jobu_runs AS runs JOIN jobu_attempts AS attempts ON attempts.run_id = runs.id "
        "LEFT JOIN jobu_attempt_output AS output ON output.run_id = attempts.run_id "
        "AND output.attempt_number = attempts.attempt_number ORDER BY runs.id, attempts.attempt_number";

    sqlite3_stmt* raw_statement{nullptr};
    auto const    prepared = sqlite3_prepare_v2(database, sql, -1, &raw_statement, nullptr);
    auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw_statement, sqlite3_finalize};
    if (prepared == SQLITE_BUSY || prepared == SQLITE_LOCKED) {
        return DurableProbeResult::Waiting;
    }
    if (prepared != SQLITE_OK) {
        reason = sqlite3_errmsg(database);
        return DurableProbeResult::Invalid;
    }

    auto const stepped = sqlite3_step(statement.get());
    if (stepped == SQLITE_DONE || stepped == SQLITE_BUSY || stepped == SQLITE_LOCKED) {
        return DurableProbeResult::Waiting;
    }
    if (stepped != SQLITE_ROW) {
        reason = sqlite3_errmsg(database);
        return DurableProbeResult::Invalid;
    }

    auto const run_state     = column_text(statement.get(), 0);
    auto const attempt_state = column_text(statement.get(), 1);
    if (run_state != "failed" || attempt_state != "completed") {
        return DurableProbeResult::Waiting;
    }

    auto const outcome = column_text(statement.get(), 2);
    auto const result  = column_text(statement.get(), 3);
    auto const body    = column_blob(statement.get(), 4);
    auto const headers = column_blob(statement.get(), 5);
    auto const valid   = outcome == "failed" && result.find(R"("outcome":"unexpected_status")") != std::string::npos &&
                         result.find(R"("status":404)") != std::string::npos && body == "daemon-output" &&
                         headers.find("X-Stage: daemon") != std::string::npos &&
                         sqlite3_column_int(statement.get(), 6) == 0 && sqlite3_column_int(statement.get(), 7) == 0 &&
                         sqlite3_column_int(statement.get(), 8) == 0;
    if (!valid) {
        reason = "terminal attempt or output fields were unexpected";
        return DurableProbeResult::Invalid;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        reason = "more than one attempt was visible";
        return DurableProbeResult::Invalid;
    }
    return DurableProbeResult::Complete;
}

auto wait_for_durable_completion(std::filesystem::path const& database_path, std::string& reason) -> bool
{
    sqlite3*   raw_database{nullptr};
    auto const opened   = sqlite3_open_v2(database_path.c_str(), &raw_database, SQLITE_OPEN_READONLY, nullptr);
    auto       database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{raw_database, sqlite3_close};
    if (opened != SQLITE_OK) {
        reason = raw_database ? sqlite3_errmsg(raw_database) : "unable to allocate a SQLite connection";
        return false;
    }
    static_cast<void>(sqlite3_busy_timeout(database.get(), 100));

    auto const deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto const probe = probe_durable_completion(database.get(), reason);
        if (probe == DurableProbeResult::Complete) {
            return true;
        }
        if (probe == DurableProbeResult::Invalid) {
            return false;
        }
        // The durable state above is the correctness predicate; this delay only bounds probe frequency.
        std::this_thread::sleep_for(10ms);
    }
    reason = "durable completion did not commit before the deadline";
    return false;
}

auto bytes(std::string_view value) -> jb::core::ByteBuffer
{
    return {reinterpret_cast<std::byte const*>(value.data()),
            reinterpret_cast<std::byte const*>(value.data() + value.size())};
}

auto run_json(std::filesystem::path const&            executable,
              std::filesystem::path const&            socket_path,
              std::initializer_list<std::string_view> arguments) -> std::optional<jb::core::JsonValue>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!successful(result) || !result.error_output.empty()) {
        fmt::print(stderr, "{}{}", result.output, result.error_output);
        return std::nullopt;
    }
    auto parsed = jb::core::parse_json(result.output);
    if (!parsed || !parsed->is_object()) {
        return std::nullopt;
    }
    return std::move(parsed).value();
}

auto write_request(std::filesystem::path const& path, jb::core::JsonValue const& request) -> bool
{
    auto encoded = jb::core::serialize_json(request);
    if (!encoded) {
        return false;
    }
    auto output = std::ofstream{path};
    output << *encoded;
    output.close();
    return !output.fail();
}

auto job_run(std::filesystem::path const& executable, std::filesystem::path const& socket_path, std::string_view job_id)
    -> std::optional<jb::core::JsonValue>
{
    auto listed = run_json(executable, socket_path, {"--json", "run", "list", "--job-id", job_id});
    if (!listed) {
        return std::nullopt;
    }
    auto const& items = listed->as_object().at("items").as_array();
    if (items.size() != 1U) {
        return std::nullopt;
    }
    return run_json(executable, socket_path, {"--json", "run", "get", items.front().as_object().at("id").as_string()});
}

auto method_rejection(CommandResult const& result, bool json) -> bool
{
    if (!failed(result) || WEXITSTATUS(result.status) != 1 || !result.output.empty()) {
        return false;
    }
    auto message = result.error_output;
    if (json) {
        auto parsed = jb::core::parse_json(message);
        if (!parsed || !parsed->is_object()) {
            return false;
        }
        auto const& error = parsed->as_object().at("error").as_object();
        if (error.at("kind").as_string() != "remote" || error.at("code").as_string() != "jobu.job.invalid_payload" ||
            error.at("category").as_string() != "invalid_argument" || error.at("outcome_unknown").as_bool()) {
            return false;
        }
        message = error.at("message").as_string();
    }
    else if (message.find("jobu.job.invalid_payload") == std::string::npos) {
        return false;
    }

    // The diagnostic may quote the bounded method, but must never expose unrelated payload fields or raw controls.
    return message.find("Supported methods: GET, POST, PUT, PATCH, DELETE, HEAD.") != std::string::npos &&
           message.find("private-marker") == std::string::npos && message.find("GET\n") == std::string::npos &&
           message.find(std::string(65, 'Z')) == std::string::npos && message.size() < 300U;
}

auto verify_http_submissions(std::filesystem::path const& executable,
                             std::filesystem::path const& socket_path,
                             std::filesystem::path const& directory,
                             jb::test::HttpTestServer&    server) -> bool
{
    using jb::core::JsonValue;

    auto const flag_url = server.url("/flag-method");
    auto       flag_job = run_json(executable,
                                   socket_path,
                                   {"--json",
                                    "job",
                                    "create",
                                    "--queue-name",
                                    "daemon-http",
                                    "--type",
                                    "http",
                                    "--now",
                                    "--url",
                                    flag_url,
                                    "--method",
                                    "pOsT",
                                    "--body",
                                    "flag-body"});
    if (!flag_job || flag_job->as_object().at("payload").as_object().at("method").as_string() != "POST" ||
        !server.wait_for_requests(2, 5s)) {
        return false;
    }
    auto const requests = server.requests();
    if (requests.size() != 2U || requests[1].method != "POST" || requests[1].target != "/flag-method" ||
        requests[1].body != bytes("flag-body")) {
        return false;
    }
    auto const flag_run = job_run(executable, socket_path, flag_job->as_object().at("id").as_string());
    if (!flag_run || flag_run->as_object().at("payload") != flag_job->as_object().at("payload")) {
        return false;
    }

    // Keep this occurrence in the future until a revision-checked replacement explicitly makes it due.
    auto request = jb::core::parse_json(
        R"({"queue_name":"daemon-http","type":"http","schedule":{"kind":"once","at":"2100-01-01T00:00:00.000000Z"},"payload":{}})");
    if (!request) {
        return false;
    }
    auto& create_fields = std::get<JsonValue::Object>(request->data);
    create_fields.at("payload") =
        JsonValue{.data = JsonValue::Object{{"url", JsonValue{.data = server.url("/file-method")}}}};
    auto const path = directory / "http-request.json";
    if (!write_request(path, *request)) {
        return false;
    }
    auto created = run_json(executable, socket_path, {"--json", "job", "create", "--request-file", path.string()});
    if (!created || created->as_object().at("payload").as_object().at("method").as_string() != "GET") {
        return false;
    }
    auto const job_id      = created->as_object().at("id").as_string();
    auto const initial_run = job_run(executable, socket_path, job_id);
    if (!initial_run || initial_run->as_object().at("payload") != created->as_object().at("payload")) {
        return false;
    }

    auto update = jb::core::parse_json(
        R"({"expected_revision":1,"payload":{"url":"http://private-marker.test/","headers":[{"name":"Authorization","value":"header-private-marker"}],"body":{"encoding":"utf8","data":"body-private-marker"}}})");
    if (!update) {
        return false;
    }
    auto& update_fields = std::get<JsonValue::Object>(update->data);
    update_fields.emplace("job_id", JsonValue{.data = job_id});
    auto& payload = std::get<JsonValue::Object>(update_fields.at("payload").data);
    for (auto const& method : {JsonValue{.data = std::string{"OPTIONS"}},
                               JsonValue{.data = std::string(512, 'Z')},
                               JsonValue{.data = std::string{"GET\n"}},
                               JsonValue{},
                               JsonValue{.data = false}}) {
        payload.insert_or_assign("method", method);
        if (!write_request(path, *update)) {
            return false;
        }
        auto human = run_jobuctl(executable, socket_path, {"job", "update", "--request-file", path.string()});
        auto json  = run_jobuctl(executable, socket_path, {"--json", "job", "update", "--request-file", path.string()});
        if (!method_rejection(human, false) || !method_rejection(json, true)) {
            fmt::print(stderr, "{}{}", human.error_output, json.error_output);
            return false;
        }
    }
    auto unchanged     = run_json(executable, socket_path, {"--json", "job", "get", job_id});
    auto unchanged_run = job_run(executable, socket_path, job_id);
    if (!unchanged || *unchanged != *created || !unchanged_run || *unchanged_run != *initial_run ||
        server.requests().size() != 2U) {
        return false;
    }

    // Token-shaped unsupported flags reach central admission; malformed token syntax retains the local usage path.
    auto unsupported = run_jobuctl(executable,
                                   socket_path,
                                   {"job",
                                    "create",
                                    "--queue-name",
                                    "daemon-http",
                                    "--type",
                                    "http",
                                    "--now",
                                    "--url",
                                    flag_url,
                                    "--method",
                                    "OPTIONS"});
    auto malformed   = run_jobuctl(executable,
                                   socket_path,
                                   {"job",
                                    "create",
                                    "--queue-name",
                                    "daemon-http",
                                    "--type",
                                    "http",
                                    "--now",
                                    "--url",
                                    flag_url,
                                    "--method",
                                    "GET\n"});
    if (!method_rejection(unsupported, false) || !failed(malformed) || WEXITSTATUS(malformed.status) != 2 ||
        !malformed.output.empty() || malformed.error_output.find("invalid_method") == std::string::npos ||
        malformed.error_output.find("GET\n") != std::string::npos) {
        return false;
    }

    payload = JsonValue::Object{
        {"url",    JsonValue{.data = server.url("/file-method")}                                    },
        {"method", JsonValue{.data = std::string{"pAtCh"}}                                          },
        {"body",
         JsonValue{.data = JsonValue::Object{{"encoding", JsonValue{.data = std::string{"utf8"}}},
                                             {"data", JsonValue{.data = std::string{"file-body"}}}}}},
    };
    update_fields.emplace("schedule",
                          JsonValue{
                              .data = JsonValue::Object{
                                                        {"kind", JsonValue{.data = std::string{"once"}}},
                                                        {"at", JsonValue{.data = std::string{"2000-01-01T00:00:00.000000Z"}}},
                                                        }
    });
    if (!write_request(path, *update)) {
        return false;
    }
    auto replaced = run_json(executable, socket_path, {"--json", "job", "update", "--request-file", path.string()});
    if (!replaced || replaced->as_object().at("revision").as_uint() != 2U ||
        replaced->as_object().at("payload").as_object().at("method").as_string() != "PATCH" ||
        !server.wait_for_requests(3, 5s)) {
        return false;
    }
    auto const replaced_run   = job_run(executable, socket_path, job_id);
    auto const final_requests = server.requests();
    return replaced_run && replaced_run->as_object().at("payload") == replaced->as_object().at("payload") &&
           final_requests.size() == 3U && final_requests[2].method == "PATCH" &&
           final_requests[2].target == "/file-method" && final_requests[2].body == bytes("file-body");
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 3) {
        return fail("expected jobud and jobuctl arguments");
    }

    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const                   jobud   = std::filesystem::path{argv[1]};
    auto const                   jobuctl = std::filesystem::path{argv[2]};

    if (!rejects_usage(jobud,
                       directory.path() / "zero.sock",
                       directory.path() / "zero.sqlite",
                       {"--http-concurrency", "0"}) ||
        !rejects_usage(jobud,
                       directory.path() / "overflow.sock",
                       directory.path() / "overflow.sqlite",
                       {"--http-concurrency", "4294967296"}) ||
        !rejects_usage(jobud,
                       directory.path() / "duplicate.sock",
                       directory.path() / "duplicate.sqlite",
                       {"--http-concurrency", "1", "--http-concurrency", "2"})) {
        return fail("jobud accepted invalid HTTP concurrency options");
    }

    // These errors must be rejected by argument parsing, before either persistent resource is opened.
    auto const invalid_cli_options = std::vector<std::vector<std::string>>{
        {"--cli-concurrency", "0"},
        {"--cli-concurrency", "4294967296"},
        {"--cli-concurrency", "1", "--cli-concurrency", "2"},
        {"--cli-concurrency"},
        {"--cli-concurrency", "1x"},
        {"--cli-concurrency", "-1"},
        {"--cli-concurrency="},
        {"--allow-root-cli", "--allow-root-cli"},
        {"--allow-root-cli=true"},
        {"--allow-root-cli=false"},
        {"--allow-root-cli="},
        {"--allow-root-cli", "true"},
        {"--unknown-option"},
    };
    for (std::size_t index = 0; index < invalid_cli_options.size(); ++index) {
        auto const name = "invalid-cli-" + std::to_string(index);
        if (!rejects_usage(jobud,
                           directory.path() / (name + ".sock"),
                           directory.path() / (name + ".sqlite"),
                           invalid_cli_options[index])) {
            return fail("jobud accepted invalid CLI startup options or opened resources before rejection");
        }
    }

    if (!rejects_client_options(jobud,
                                directory.path() / "proxy.sock",
                                directory.path() / "proxy.sqlite",
                                "--http-proxy",
                                "ftp://example.test") ||
        !rejects_client_options(jobud,
                                directory.path() / "ca.sock",
                                directory.path() / "ca.sqlite",
                                "--http-ca-bundle",
                                (directory.path() / "missing-ca.pem").string())) {
        return fail("jobud did not reject invalid HTTP client options before listening");
    }

    jb::test::HttpTestServer server;
    server.enqueue_response({
        .status_code = 404,
        .reason      = "Not Found",
        .headers     = {{.name = "X-Stage", .value = "daemon"}},
        .body        = bytes("daemon-output"),
    });
    server.release_responses();

    auto const database_path = directory.path() / "jobu.sqlite";
    auto const socket_path   = directory.path() / "jobud.sock";
    auto       daemon        = spawn_jobud(jobud, socket_path, database_path, 1);
    if (!daemon) {
        return fail("unable to start jobud");
    }
    if (!wait_for_listener(*daemon, socket_path)) {
        return fail("jobud did not begin listening before the deadline");
    }
    if (!run_success(jobuctl, socket_path, {"system", "info"})) {
        return fail("jobud did not complete the readiness RPC");
    }
    if (!run_success(jobuctl, socket_path, {"queue", "create", "daemon-http"})) {
        return fail("jobuctl could not create the HTTP queue");
    }

    auto const url = server.url("/stage-5-16");
    if (!run_success(jobuctl,
                     socket_path,
                     {"job",
                      "create",
                      "--queue-name",
                      "daemon-http",
                      "--type",
                      "http",
                      "--at",
                      "2000-01-01T00:00:00.000000Z",
                      "--url",
                      url})) {
        return fail("jobuctl could not create the due HTTP job");
    }

    if (!server.wait_for_requests(1, 5s)) {
        return fail("the management mutation did not wake HTTP scheduling");
    }
    auto const requests = server.requests();
    if (requests.size() != 1U || requests.front().method != "GET" || requests.front().target != "/stage-5-16") {
        return fail("jobud sent an unexpected HTTP request");
    }

    auto durable_error = std::string{};
    if (!wait_for_durable_completion(database_path, durable_error)) {
        fmt::print(stderr, "{}\n", durable_error);
        return fail("jobud did not commit the expected HTTP completion and output");
    }
    if (!verify_http_submissions(jobuctl, socket_path, directory.path(), server)) {
        return fail("HTTP CLI submission, diagnostic, snapshot, or wire verification failed");
    }
    if (daemon->try_wait()) {
        return fail("jobud exited after completing the HTTP attempt");
    }
    daemon->terminate();

    auto const cleanup_error = directory.cleanup();
    if (cleanup_error) {
        return fail("temporary directory cleanup failed");
    }
    return EXIT_SUCCESS;
}
