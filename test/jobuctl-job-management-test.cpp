#include "support/temporary_directory.hpp"

#include "database.hpp"
#include "json.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "uuid.hpp"
#include "value.hpp"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

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

class HttpSentinel {
public:
    static auto create() -> std::optional<HttpSentinel>
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return std::nullopt;
        }

        auto const status_flags = ::fcntl(fd, F_GETFL, 0);
        if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
            static_cast<void>(::close(fd));
            return std::nullopt;
        }

        auto const descriptor_flags = ::fcntl(fd, F_GETFD, 0);
        if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
            static_cast<void>(::close(fd));
            return std::nullopt;
        }

        auto address            = sockaddr_in{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, 1) < 0) {
            static_cast<void>(::close(fd));
            return std::nullopt;
        }

        auto length = socklen_t{sizeof(address)};
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
            static_cast<void>(::close(fd));
            return std::nullopt;
        }
        return HttpSentinel{fd, ntohs(address.sin_port)};
    }

    ~HttpSentinel()
    {
        if (_fd >= 0) {
            static_cast<void>(::close(_fd));
        }
    }

    HttpSentinel(HttpSentinel const&)                    = delete;
    auto operator=(HttpSentinel const&) -> HttpSentinel& = delete;

    HttpSentinel(HttpSentinel&& other) noexcept
        : _fd{std::exchange(other._fd, -1)}
        , _port{other._port}
    {}

    auto operator=(HttpSentinel&& other) noexcept -> HttpSentinel&
    {
        if (this != &other) {
            if (_fd >= 0) {
                static_cast<void>(::close(_fd));
            }
            _fd   = std::exchange(other._fd, -1);
            _port = other._port;
        }
        return *this;
    }

    [[nodiscard]] auto url() const -> std::string
    {
        return fmt::format("http://127.0.0.1:{}/stage-3-17-sentinel", _port);
    }

    [[nodiscard]] auto received_connection() const -> bool
    {
        for (;;) {
            auto const client = ::accept(_fd, nullptr, nullptr);
            if (client >= 0) {
                static_cast<void>(::close(client));
                return true;
            }
            if (errno != EINTR) {
                return errno != EAGAIN && errno != EWOULDBLOCK;
            }
        }
    }

private:
    HttpSentinel(int fd, std::uint16_t port)
        : _fd{fd}
        , _port{port}
    {}

    int           _fd{-1};
    std::uint16_t _port{0};
};

struct CommandResult {
    bool        completed{false};
    int         status{0};
    std::string output;
};

auto fail(std::string_view message) -> int
{
    fmt::print(stderr, "jobuctl-job-management-test: {}\n", message);
    return EXIT_FAILURE;
}

auto spawn_jobud(std::filesystem::path const& executable,
                 std::filesystem::path const& socket_path,
                 std::filesystem::path const& database_path,
                 bool                         allow_root_cli = false) -> std::optional<ChildProcess>
{
    auto const pid = ::fork();
    if (pid < 0) {
        return std::nullopt;
    }
    if (pid == 0) {
        if (allow_root_cli) {
            ::execl(executable.c_str(),
                    executable.c_str(),
                    "--socket",
                    socket_path.c_str(),
                    "--database",
                    database_path.c_str(),
                    "--allow-root-cli",
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }

        ::execl(executable.c_str(),
                executable.c_str(),
                "--socket",
                socket_path.c_str(),
                "--database",
                database_path.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return ChildProcess{pid};
}

auto root_cli_opted_in() -> bool
{
    auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
    return ::geteuid() == 0 && enabled != nullptr && std::string_view{enabled} == "1";
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

auto run_jobuctl(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> CommandResult
{
    int output_pipe[2];
    if (::pipe(output_pipe) < 0) {
        return {};
    }

    auto const pid = ::fork();
    if (pid < 0) {
        static_cast<void>(::close(output_pipe[0]));
        static_cast<void>(::close(output_pipe[1]));
        return {};
    }
    if (pid == 0) {
        static_cast<void>(::close(output_pipe[0]));
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 || ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(127);
        }
        static_cast<void>(::close(output_pipe[1]));

        auto owned_arguments = std::vector<std::string>{
            executable.string(),
            "--socket",
            socket_path.string(),
        };
        owned_arguments.reserve(owned_arguments.size() + arguments.size());
        for (auto const argument : arguments) {
            owned_arguments.emplace_back(argument);
        }

        auto exec_arguments = std::vector<char*>{};
        exec_arguments.reserve(owned_arguments.size() + 1U);
        for (auto& argument : owned_arguments) {
            exec_arguments.push_back(argument.data());
        }
        exec_arguments.push_back(nullptr);
        ::execv(executable.c_str(), exec_arguments.data());
        ::_exit(127);
    }

    static_cast<void>(::close(output_pipe[1]));
    auto const flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (flags < 0 || ::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        static_cast<void>(::close(output_pipe[0]));
        ChildProcess child{pid};
        return {};
    }

    ChildProcess  command{pid};
    CommandResult result;
    auto          eof      = false;
    auto const    deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        read_available(output_pipe[0], result.output, eof);
        auto const status = command.try_wait();
        if (status && eof) {
            result.completed = true;
            result.status    = *status;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    static_cast<void>(::close(output_pipe[0]));
    return result;
}

auto successful(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_SUCCESS;
}

auto failed(CommandResult const& result) -> bool
{
    return result.completed && WIFEXITED(result.status) && WEXITSTATUS(result.status) != EXIT_SUCCESS;
}

auto run_success(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> std::optional<std::string>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!successful(result)) {
        fmt::print(stderr, "{}", result.output);
        return std::nullopt;
    }
    return std::move(result.output);
}

auto run_failure(std::filesystem::path const&            executable,
                 std::filesystem::path const&            socket_path,
                 std::initializer_list<std::string_view> arguments) -> std::optional<std::string>
{
    auto result = run_jobuctl(executable, socket_path, arguments);
    if (!failed(result)) {
        fmt::print(stderr, "{}", result.output);
        return std::nullopt;
    }
    return std::move(result.output);
}

auto queue_id_from_summary(std::string_view output) -> std::optional<std::string>
{
    constexpr auto prefix = std::string_view{"Queue "};
    if (!output.starts_with(prefix)) {
        return std::nullopt;
    }
    auto const separator = output.find(':', prefix.size());
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    auto parsed = jb::core::Uuid::parse(output.substr(prefix.size(), separator - prefix.size()));
    if (!parsed) {
        return std::nullopt;
    }
    return parsed->to_string();
}

auto job_id_from_summary(std::string_view output) -> std::optional<std::string>
{
    constexpr auto prefix = std::string_view{"Job "};
    if (!output.starts_with(prefix)) {
        return std::nullopt;
    }
    auto const separator = output.find(':', prefix.size());
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    auto parsed = jb::core::Uuid::parse(output.substr(prefix.size(), separator - prefix.size()));
    if (!parsed) {
        return std::nullopt;
    }
    return parsed->to_string();
}

auto revision_from_summary(std::string_view output) -> std::optional<std::uint64_t>
{
    constexpr auto prefix = std::string_view{", revision="};
    auto const     begin  = output.find(prefix);
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    auto const value_begin = begin + prefix.size();
    auto const end         = output.find(',', value_begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }

    auto revision = std::uint64_t{};
    auto parsed   = std::from_chars(output.data() + value_begin, output.data() + end, revision);
    if (parsed.ec != std::errc{} || parsed.ptr != output.data() + end) {
        return std::nullopt;
    }
    return revision;
}

auto job_summary(std::string_view id,
                 std::string_view queue_id,
                 std::uint64_t    revision,
                 std::string_view name,
                 std::string_view state,
                 std::string_view type,
                 std::string_view at,
                 std::int32_t     priority) -> std::string
{
    return fmt::format("Job {}: queue_id={}, revision={}, name={}, state={}, type={}, at={}, priority={}\n",
                       id,
                       queue_id,
                       revision,
                       name,
                       state,
                       type,
                       at,
                       priority);
}

auto rejects_before_connect(std::filesystem::path const&            executable,
                            std::filesystem::path const&            socket_path,
                            std::initializer_list<std::string_view> arguments) -> bool
{
    auto output = run_failure(executable, socket_path, arguments);
    return output && output->find("Usage:\n") != std::string::npos;
}

template <typename Value>
auto field(jb::db::Query const& query, std::string_view name) -> Value const*
{
    auto const* value = query.value(name);
    return value ? std::get_if<Value>(value) : nullptr;
}

auto expected_blob(jb::core::Uuid const& value) -> jb::core::ByteBuffer
{
    return {value.bytes().begin(), value.bytes().end()};
}

auto inspect_job(jb::db::Database& database,
                 std::string_view  job_id_text,
                 std::string_view  queue_id_text,
                 std::int64_t      revision,
                 std::string_view  job_state,
                 std::string_view  run_state,
                 std::string_view  expected_payload) -> bool
{
    auto job_id   = jb::core::Uuid::parse(job_id_text);
    auto queue_id = jb::core::Uuid::parse(queue_id_text);
    if (!job_id || !queue_id) {
        return false;
    }

    jb::db::Query query{database};
    if (!query.prepare(
            "SELECT jobs.queue_id AS queue_id, jobs.revision AS job_revision, jobs.state AS job_state, "
            "jobs.payload_json AS job_payload, runs.state AS run_state, runs.schedule_owned AS schedule_owned, "
            "runs.payload_json AS run_payload FROM jobu_jobs AS jobs "
            "JOIN jobu_runs AS runs ON runs.job_id = jobs.id WHERE jobs.id = :job_id ORDER BY runs.id") ||
        !query.bind_value(":job_id", jb::db::make_blob(job_id->bytes())) || !query.exec()) {
        return false;
    }
    auto row = query.next();
    if (!row || !*row) {
        return false;
    }

    auto const* stored_queue     = field<jb::core::ByteBuffer>(query, "queue_id");
    auto const* stored_revision  = field<std::int64_t>(query, "job_revision");
    auto const* stored_job_state = field<std::string>(query, "job_state");
    auto const* stored_run_state = field<std::string>(query, "run_state");
    auto const* schedule_owned   = field<std::int64_t>(query, "schedule_owned");
    auto const* job_payload      = field<std::string>(query, "job_payload");
    auto const* run_payload      = field<std::string>(query, "run_payload");
    if (stored_queue == nullptr || *stored_queue != expected_blob(*queue_id) || stored_revision == nullptr ||
        *stored_revision != revision || stored_job_state == nullptr || *stored_job_state != job_state ||
        stored_run_state == nullptr || *stored_run_state != run_state || schedule_owned == nullptr ||
        *schedule_owned != 1 || job_payload == nullptr || run_payload == nullptr || *job_payload != expected_payload ||
        *run_payload != expected_payload) {
        return false;
    }

    auto more = query.next();
    return more && !*more && query.finish();
}

auto scalar_count(jb::db::Database& database, std::string_view table) -> std::optional<std::int64_t>
{
    jb::db::Query query{database};
    if (!query.exec("SELECT COUNT(*) AS row_count FROM " + std::string{table})) {
        return std::nullopt;
    }
    auto row = query.next();
    if (!row || !*row) {
        return std::nullopt;
    }
    auto const* count = field<std::int64_t>(query, "row_count");
    if (!count) {
        return std::nullopt;
    }
    auto result = *count;
    if (!query.finish()) {
        return std::nullopt;
    }
    return result;
}

auto inspect_database(std::filesystem::path const& database_path,
                      std::string_view             cli_job_id,
                      std::string_view             cli_queue_id,
                      std::int64_t                 cli_revision,
                      std::string_view             cli_payload_marker,
                      std::string_view             http_job_id,
                      std::string_view             http_queue_id,
                      std::string_view             http_url) -> bool
{
    auto database = jb::db::Database{
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = database_path})};
    if (!database.open()) {
        return false;
    }

    auto const cli_payload = fmt::format(
        R"({{"arguments":["-c","{}"],"command":"/bin/sh","environment":{{"EMPTY":"","REMOVE":null,"VALUE":"one=two"}},"expected_exit_codes":[0,7],"working_directory":"/"}})",
        cli_payload_marker);
    auto const http_payload = fmt::format(R"({{"method":"GET","url":"{}"}})", http_url);
    auto valid = inspect_job(database, cli_job_id, cli_queue_id, cli_revision, "deleted", "cancelled", cli_payload) &&
                 inspect_job(database, http_job_id, http_queue_id, 1, "active", "scheduled", http_payload);
    auto const run_count     = scalar_count(database, "jobu_runs");
    auto const attempt_count = scalar_count(database, "jobu_attempts");
    valid                    = valid && run_count && *run_count == 2 && attempt_count && *attempt_count == 0;
    auto const closed        = database.close();
    return valid && closed;
}

/// Exercises the CLI against retained daemon state while the private integration daemon is serving.
auto verify_run_attempt_cli(std::filesystem::path const& executable,
                            std::filesystem::path const& socket,
                            std::filesystem::path const& directory,
                            bool                         can_execute_cli) -> bool
{
    if (!run_success(executable, socket, {"queue", "create", "history"})) {
        return false;
    }
    // A future schedule-owned occurrence can be cancelled before any attempt starts.
    auto pending_job  = run_success(executable,
                                    socket,
                                    {"job",
                                     "create",
                                     "--queue-name",
                                     "history",
                                     "--type",
                                     "cli",
                                     "--at",
                                     "2030-01-01T00:00:00Z",
                                     "--command",
                                     "/bin/true",
                                     "--json"});
    auto pending_json = jb::core::parse_json(pending_job.value_or(""));
    if (!pending_json) {
        return false;
    }
    auto const pending_job_id = pending_json->as_object().at("id").as_string();
    auto       pending_page   = run_success(executable, socket, {"run", "list", "--job-id", pending_job_id, "--json"});
    auto       pending_page_json = jb::core::parse_json(pending_page.value_or(""));
    if (!pending_page_json || pending_page_json->as_object().at("items").as_array().size() != 1U) {
        return false;
    }
    auto const& pending_summary = pending_page_json->as_object().at("items").as_array().front().as_object();
    if (pending_summary.contains("payload") || pending_summary.contains("output")) {
        return false;
    }
    auto const pending_id     = pending_summary.at("id").as_string();
    auto       cancelled      = run_success(executable, socket, {"run", "cancel", pending_id, "--wait", "--json"});
    auto       cancelled_json = jb::core::parse_json(cancelled.value_or(""));
    if (!cancelled_json || cancelled_json->as_object().at("disposition").as_string() != "completed" ||
        cancelled_json->as_object().at("run").as_object().at("state").as_string() != "cancelled") {
        return false;
    }
    auto pending_attempts      = run_success(executable, socket, {"attempt", "list", pending_id, "--json"});
    auto pending_attempts_json = jb::core::parse_json(pending_attempts.value_or(""));
    if (!pending_attempts_json || !pending_attempts_json->as_object().at("items").as_array().empty()) {
        return false;
    }

    // Root CI execution requires an explicit opt-in; the pending-run checks remain useful without it.
    if (!can_execute_cli) {
        return true;
    }

    // Run Now leaves the scheduled occurrence intact and exposes its own retained attempt and output.
    auto output_job      = run_success(executable,
                                       socket,
                                       {"job",
                                        "create",
                                        "--queue-name",
                                        "history",
                                        "--type",
                                        "cli",
                                        "--at",
                                        "2030-02-01T00:00:00Z",
                                        "--command",
                                        "/bin/sh",
                                        "--arg=-c",
                                        "--arg=printf 'A\\000\\377Z'",
                                        "--attribute",
                                        R"(output.capture="always")",
                                        "--json"});
    auto output_job_json = jb::core::parse_json(output_job.value_or(""));
    if (!output_job_json) {
        return false;
    }
    auto const output_job_id = output_job_json->as_object().at("id").as_string();
    auto started      = run_success(executable,
                                    socket,
                                    {"job", "run-now", output_job_id, "--idempotency-key", "history-manual", "--json"});
    auto started_json = jb::core::parse_json(started.value_or(""));
    if (!started_json) {
        return false;
    }
    auto const run_id = started_json->as_object().at("id").as_string();
    auto       replay = run_success(executable,
                                    socket,
                                    {"job", "run-now", output_job_id, "--idempotency-key", "history-manual", "--json"});
    auto       replay_json = jb::core::parse_json(replay.value_or(""));
    if (!replay_json || replay_json->as_object().at("id").as_string() != run_id) {
        return false;
    }

    // The subprocess is asynchronous; each read observes durable state without delaying the daemon event loop.
    auto       succeeded  = false;
    auto       last_state = std::string{"not observed"};
    auto const deadline   = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = run_success(executable, socket, {"run", "get", run_id, "--json"});
        auto value    = jb::core::parse_json(snapshot.value_or(""));
        if (!value) {
            return false;
        }
        auto const& state = value->as_object().at("state").as_string();
        last_state        = state;
        if (state == "succeeded") {
            succeeded = true;
            break;
        }
        if (state == "failed" || state == "interrupted" || state == "cancelled") {
            fmt::print(stderr, "run-now {} reached state {}\n", run_id, state);
            return false;
        }
    }
    if (!succeeded) {
        fmt::print(stderr, "run-now {} remained in state {} until the deadline\n", run_id, last_state);
        return false;
    }

    // Pages contain summaries only; details and raw output require separate requests.
    auto page = run_success(executable, socket, {"run", "list", "--job-id", output_job_id, "--limit", "1", "--json"});
    auto page_json = jb::core::parse_json(page.value_or(""));
    if (!page_json || page_json->as_object().at("items").as_array().size() != 1U ||
        !page_json->as_object().at("next_cursor").is_string()) {
        return false;
    }
    auto const& first_summary = page_json->as_object().at("items").as_array().front().as_object();
    if (first_summary.contains("payload") || first_summary.contains("output")) {
        return false;
    }
    auto const cursor    = page_json->as_object().at("next_cursor").as_string();
    auto       next      = run_success(executable, socket, {"run", "list", "--cursor", cursor, "--json"});
    auto       next_json = jb::core::parse_json(next.value_or(""));
    if (!next_json || next_json->as_object().at("items").as_array().size() != 1U ||
        next_json->as_object().at("items").as_array().front().as_object().at("id").as_string() ==
            first_summary.at("id").as_string()) {
        return false;
    }

    auto attempts      = run_success(executable, socket, {"attempt", "list", run_id, "--json"});
    auto attempts_json = jb::core::parse_json(attempts.value_or(""));
    if (!attempts_json || attempts_json->as_object().at("items").as_array().size() != 1U) {
        return false;
    }
    auto const& attempt_summary = attempts_json->as_object().at("items").as_array().front().as_object();
    if (attempt_summary.contains("result") || attempt_summary.contains("output")) {
        return false;
    }
    auto details      = run_success(executable, socket, {"attempt", "get", run_id, "1", "--json"});
    auto details_json = jb::core::parse_json(details.value_or(""));
    if (!details_json || details_json->as_object().at("state").as_string() != "completed") {
        return false;
    }

    auto output = run_success(executable, socket, {"attempt", "output", run_id, "1", "--channel", "stdout", "--json"});
    auto output_json = jb::core::parse_json(output.value_or(""));
    if (!output_json || output_json->as_object().at("status").as_string() != "available" ||
        output_json->as_object().at("bytes_returned").as_uint() != 4) {
        return false;
    }
    auto const expected = std::string{"A\0\xffZ", 4};
    auto raw = run_success(executable, socket, {"attempt", "output", run_id, "1", "--channel", "stdout", "--raw"});
    if (!raw || *raw != expected) {
        return false;
    }
    auto const path = directory / "retained-chunk.bin";
    auto saved = run_success(executable,
                             socket,
                             {"attempt", "output", run_id, "1", "--channel", "stdout", "--output-file", path.string()});
    if (!saved || !saved->empty()) {
        return false;
    }
    auto file  = std::ifstream{path, std::ios::binary};
    auto bytes = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (bytes != expected) {
        return false;
    }
    if (!run_failure(executable,
                     socket,
                     {"attempt", "output", run_id, "1", "--channel", "stdout", "--output-file", path.string()})) {
        return false;
    }

    // A FIFO holds a real subprocess attempt after it signals readiness, so cancellation starts while active.
    auto const fifo  = directory / "active-run.fifo";
    auto const ready = directory / "active-run.ready";
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        return false;
    }
    auto script          = fmt::format("printf ready > '{}'; cat '{}'", ready.string(), fifo.string());
    auto active_job      = run_success(executable,
                                       socket,
                                       {"job",
                                        "create",
                                        "--queue-name",
                                        "history",
                                        "--type",
                                        "cli",
                                        "--at",
                                        "2030-03-01T00:00:00Z",
                                        "--command",
                                        "/bin/sh",
                                        "--arg=-c",
                                        "--arg",
                                        script,
                                        "--attribute",
                                        "cli.termination_grace=0",
                                        "--json"});
    auto active_job_json = jb::core::parse_json(active_job.value_or(""));
    if (!active_job_json) {
        return false;
    }
    auto const active_job_id   = active_job_json->as_object().at("id").as_string();
    auto       active_run      = run_success(executable, socket, {"job", "run-now", active_job_id, "--json"});
    auto       active_run_json = jb::core::parse_json(active_run.value_or(""));
    if (!active_run_json) {
        return false;
    }
    auto const active_run_id  = active_run_json->as_object().at("id").as_string();
    auto const ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (!std::filesystem::exists(ready)) {
        return false;
    }
    auto active_cancel      = run_success(executable, socket, {"run", "cancel", active_run_id, "--wait", "--json"});
    auto active_cancel_json = jb::core::parse_json(active_cancel.value_or(""));
    return active_cancel_json && active_cancel_json->as_object().at("disposition").as_string() == "completed" &&
           active_cancel_json->as_object().at("run").as_object().at("state").as_string() == "cancelled";
}

} // anonymous namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 3) {
        return fail("expected jobud and jobuctl arguments");
    }

    jb::test::TemporaryDirectory directory;
    auto const                   database_path = directory.path() / "jobu.sqlite";
    auto const                   socket_path   = directory.path() / "jobud-fresh.sock";

    if (!rejects_before_connect(argv[2],
                                socket_path,
                                {"job",
                                 "create",
                                 "--queue-id",
                                 "bad",
                                 "--queue-name",
                                 "source",
                                 "--type",
                                 "cli",
                                 "--at",
                                 "2030-01-01T00:00:00Z",
                                 "--command",
                                 "/bin/true"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job",
                                 "create",
                                 "--queue-name",
                                 "source",
                                 "--type",
                                 "cli",
                                 "--at",
                                 "not-utc",
                                 "--command",
                                 "/bin/true"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job",
                                 "create",
                                 "--queue-name",
                                 "source",
                                 "--type",
                                 "cli",
                                 "--at",
                                 "2030-01-01T00:00:00Z",
                                 "--url",
                                 "http://example.test"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job",
                                 "create",
                                 "--queue-name",
                                 "source",
                                 "--type",
                                 "http",
                                 "--at",
                                 "2030-01-01T00:00:00Z",
                                 "--url",
                                 "http://example.test",
                                 "--priority",
                                 "-2147483649"}) ||
        !rejects_before_connect(argv[2], socket_path, {"job", "update", "bad", "--revision", "1", "--priority", "1"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job",
                                 "update",
                                 "00000000-0000-7000-8000-000000000001",
                                 "--revision",
                                 "1",
                                 "--name",
                                 "name",
                                 "--clear-name"}) ||
        !rejects_before_connect(
            argv[2],
            socket_path,
            {"job", "list", "--queue-id", "00000000-0000-7000-8000-000000000001", "--queue-name", "source"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job", "move", "00000000-0000-7000-8000-000000000001", "--revision", "1"}) ||
        !rejects_before_connect(argv[2],
                                socket_path,
                                {"job", "delete", "00000000-0000-7000-8000-000000000001", "--revision", "0"})) {
        return fail("jobuctl accepted invalid job command arguments");
    }

    for (auto const& options : {
             std::initializer_list<std::string_view>{"job",
                                                     "create", "--queue-name",
                                                     "source", "--type",
                                                     "cli", "--at",
                                                     "2030-01-01T00:00:00Z", "--command",
                                                     "/bin/true", "--env",
                                                     "NAME=value", "--unset-env",
                                                     "NAME"},
             std::initializer_list<std::string_view>{"job",
                                                     "create", "--queue-name",
                                                     "source", "--type",
                                                     "cli", "--at",
                                                     "2030-01-01T00:00:00Z", "--command",
                                                     "/bin/true", "--working-directory",
                                                     "relative"},
    }) {
        if (!rejects_before_connect(argv[2], socket_path, options)) {
            return fail("jobuctl accepted invalid CLI creation fields");
        }
    }

    auto http_sentinel = HttpSentinel::create();
    if (!http_sentinel) {
        return fail("unable to create the HTTP execution sentinel");
    }
    auto const http_url       = http_sentinel->url();
    auto const marker_path    = directory.path() / "cli-runner-executed";
    auto const marker_command = fmt::format("touch {}", marker_path.string());
    auto const cli_at         = std::string{"2030-01-01T02:03:04.000000Z"};
    auto const updated_at     = std::string{"2030-02-03T04:05:06.000000Z"};
    auto const http_at        = std::string{"2030-03-04T05:06:07.000000Z"};

    auto daemon = spawn_jobud(argv[1], socket_path, database_path);
    if (!daemon) {
        return fail("unable to start jobud");
    }
    if (!wait_for_listener(*daemon, socket_path)) {
        return fail("jobud did not begin listening before the deadline");
    }

    auto source = run_success(argv[2], socket_path, {"queue", "create", "source"});
    auto target = run_success(argv[2], socket_path, {"queue", "create", "target"});
    if (!source || !target) {
        return fail("jobuctl could not create the job queues");
    }
    auto const source_id = queue_id_from_summary(*source);
    auto const target_id = queue_id_from_summary(*target);
    if (!source_id || !target_id) {
        return fail("queue creation did not print canonical UUIDs");
    }

    auto cli_created = run_success(argv[2],
                                   socket_path,
                                   {"job",
                                    "create",
                                    "--queue-name",
                                    "source",
                                    "--type",
                                    "cli",
                                    "--at",
                                    cli_at,
                                    "--command",
                                    "/bin/sh",
                                    "--arg",
                                    "-c",
                                    "--arg",
                                    marker_command,
                                    "--working-directory",
                                    "/",
                                    "--env",
                                    "EMPTY=",
                                    "--env",
                                    "VALUE=one=two",
                                    "--unset-env",
                                    "REMOVE",
                                    "--expected-exit-code",
                                    "0",
                                    "--expected-exit-code",
                                    "7",
                                    "--name",
                                    "cli-job",
                                    "--priority",
                                    "-7",
                                    "--idempotency-key",
                                    "create-cli-job"});
    if (!cli_created) {
        return fail("jobuctl could not create the CLI job");
    }
    auto const cli_id = job_id_from_summary(*cli_created);
    if (!cli_id || *cli_created != job_summary(*cli_id, *source_id, 1, "cli-job", "active", "cli", cli_at, -7)) {
        fmt::print(stderr, "{}", *cli_created);
        return fail("CLI job creation printed an unexpected summary");
    }

    auto cli_replayed = run_success(argv[2],
                                    socket_path,
                                    {"job",
                                     "create",
                                     "--queue-name",
                                     "source",
                                     "--type",
                                     "cli",
                                     "--at",
                                     cli_at,
                                     "--command",
                                     "/bin/sh",
                                     "--arg",
                                     "-c",
                                     "--arg",
                                     marker_command,
                                     "--working-directory",
                                     "/",
                                     "--env",
                                     "EMPTY=",
                                     "--env",
                                     "VALUE=one=two",
                                     "--unset-env",
                                     "REMOVE",
                                     "--expected-exit-code",
                                     "0",
                                     "--expected-exit-code",
                                     "7",
                                     "--name",
                                     "cli-job",
                                     "--priority",
                                     "-7",
                                     "--idempotency-key",
                                     "create-cli-job"});
    if (!cli_replayed || *cli_replayed != *cli_created) {
        return fail("job create did not replay the original durable result");
    }

    auto http_created = run_success(argv[2],
                                    socket_path,
                                    {"job",
                                     "create",
                                     "--queue-id",
                                     *source_id,
                                     "--type",
                                     "http",
                                     "--at",
                                     http_at,
                                     "--url",
                                     http_url,
                                     "--priority",
                                     "2"});
    if (!http_created) {
        return fail("jobuctl could not create the HTTP job");
    }
    auto const http_id = job_id_from_summary(*http_created);
    if (!http_id || *http_created != job_summary(*http_id, *source_id, 1, "<unnamed>", "active", "http", http_at, 2)) {
        fmt::print(stderr, "{}", *http_created);
        return fail("HTTP job creation printed an unexpected summary");
    }

    auto cli_get = run_success(argv[2], socket_path, {"job", "get", *cli_id});
    if (!cli_get || *cli_get != *cli_created) {
        return fail("job get did not return the created CLI job");
    }
    auto first_page = run_success(argv[2], socket_path, {"job", "list", "--queue-name", "source", "--limit", "1"});
    if (!first_page || *first_page != *cli_created + fmt::format("Next after: {}\n", *cli_id)) {
        fmt::print(stderr, "{}", first_page.value_or(""));
        return fail("bounded job list did not print its continuation cursor");
    }
    auto second_page = run_success(argv[2], socket_path, {"job", "list", "--after", *cli_id});
    if (!second_page || *second_page != *http_created) {
        fmt::print(stderr, "{}", second_page.value_or(""));
        return fail("job list did not continue after the supplied UUID");
    }

    auto conflict = run_failure(argv[2], socket_path, {"job", "update", *cli_id, "--revision", "2", "--priority", "1"});
    if (!conflict || conflict->find("(jobu.job.revision_conflict)") == std::string::npos) {
        fmt::print(stderr, "{}", conflict.value_or(""));
        return fail("stale job update did not expose its stable revision-conflict code");
    }

    auto updated = run_success(
        argv[2],
        socket_path,
        {"job", "update", *cli_id, "--revision", "1", "--clear-name", "--priority", "-12", "--at", updated_at});
    if (!updated || *updated != job_summary(*cli_id, *source_id, 2, "<unnamed>", "active", "cli", updated_at, -12)) {
        fmt::print(stderr, "{}", updated.value_or(""));
        return fail("job update did not print the committed replacement fields");
    }

    auto suspended        = run_success(argv[2], socket_path, {"job", "suspend", *cli_id, "--wait"});
    auto suspend_revision = suspended ? revision_from_summary(*suspended) : std::nullopt;
    if (!suspended || !suspend_revision || *suspend_revision <= 2 ||
        *suspended !=
            job_summary(*cli_id, *source_id, *suspend_revision, "<unnamed>", "suspended", "cli", updated_at, -12)) {
        fmt::print(stderr, "{}", suspended.value_or(""));
        return fail("job suspend did not print its returned durable revision");
    }

    auto moved = run_success(
        argv[2],
        socket_path,
        {"job", "move", *cli_id, "--revision", fmt::format("{}", *suspend_revision), "--queue-name", "target"});
    auto move_revision = moved ? revision_from_summary(*moved) : std::nullopt;
    if (!moved || !move_revision || *move_revision <= *suspend_revision ||
        *moved != job_summary(*cli_id, *target_id, *move_revision, "<unnamed>", "suspended", "cli", updated_at, -12)) {
        fmt::print(stderr, "{}", moved.value_or(""));
        return fail("job move did not use and return the carried revision");
    }

    daemon->terminate();

    auto const reopen_socket = directory.path() / "jobud-reopen.sock";
    auto       reopened      = spawn_jobud(argv[1], reopen_socket, database_path);
    if (!reopened) {
        return fail("unable to restart jobud");
    }
    if (!wait_for_listener(*reopened, reopen_socket)) {
        return fail("jobud did not reopen the durable job database before the deadline");
    }

    auto after_restart = run_success(argv[2], reopen_socket, {"job", "get", *cli_id});
    if (!after_restart || *after_restart != *moved) {
        return fail("the moved job did not survive daemon restart");
    }

    auto resumed         = run_success(argv[2], reopen_socket, {"job", "resume", *cli_id});
    auto resume_revision = resumed ? revision_from_summary(*resumed) : std::nullopt;
    if (!resumed || !resume_revision || *resume_revision <= *move_revision ||
        *resumed != job_summary(*cli_id, *target_id, *resume_revision, "<unnamed>", "active", "cli", updated_at, -12)) {
        return fail("job resume did not print its returned durable revision");
    }

    suspended        = run_success(argv[2], reopen_socket, {"job", "suspend", *cli_id});
    suspend_revision = suspended ? revision_from_summary(*suspended) : std::nullopt;
    if (!suspended || !suspend_revision || *suspend_revision <= *resume_revision ||
        *suspended !=
            job_summary(*cli_id, *target_id, *suspend_revision, "<unnamed>", "suspended", "cli", updated_at, -12)) {
        return fail("job did not suspend again before deletion");
    }

    auto deleted = run_success(argv[2],
                               reopen_socket,
                               {"job", "delete", *cli_id, "--revision", fmt::format("{}", *suspend_revision)});
    if (!deleted || *deleted != fmt::format("Deleted job id={}\n", *cli_id)) {
        return fail("job delete did not acknowledge the supplied job UUID");
    }
    auto const deleted_revision = *suspend_revision + 1U;
    auto const deleted_summary =
        job_summary(*cli_id, *target_id, deleted_revision, "<unnamed>", "deleted", "cli", updated_at, -12);

    auto listed_deleted = run_success(argv[2], reopen_socket, {"job", "list", "--include-deleted"});
    if (!listed_deleted || *listed_deleted != deleted_summary + *http_created) {
        fmt::print(stderr, "{}", listed_deleted.value_or(""));
        return fail("job list did not show the complete durable lifecycle state");
    }

    reopened->terminate();

    if (std::filesystem::exists(marker_path) || http_sentinel->received_connection()) {
        return fail("a Phase 3 runner payload was executed");
    }
    if (!inspect_database(database_path,
                          *cli_id,
                          *target_id,
                          static_cast<std::int64_t>(deleted_revision),
                          marker_command,
                          *http_id,
                          *source_id,
                          http_url)) {
        return fail("the stored jobs, schedule-owned runs, payloads, or attempts violated the Phase 3 contract");
    }

    auto const persisted_socket = directory.path() / "jobud-persisted.sock";
    // Only this daemon exercises real CLI attempts; earlier restarts retain the default root policy.
    auto const allow_root_cli   = root_cli_opted_in();
    auto       persisted        = spawn_jobud(argv[1], persisted_socket, database_path, allow_root_cli);
    if (!persisted) {
        return fail("unable to restart jobud after direct database inspection");
    }
    if (!wait_for_listener(*persisted, persisted_socket)) {
        return fail("jobud did not reopen the inspected database before the deadline");
    }
    auto persisted_list = run_success(argv[2], persisted_socket, {"job", "list", "--include-deleted"});
    if (!persisted_list || *persisted_list != deleted_summary + *http_created) {
        return fail("job lifecycle state did not persist through the final daemon restart");
    }

    auto immediate        = run_success(argv[2],
                                        persisted_socket,
                                        {"job",
                                         "create",
                                         "--queue-name",
                                         "source",
                                         "--type",
                                         "cli",
                                         "--now",
                                         "--command",
                                         "/bin/true",
                                         "--idempotency-key",
                                         "create-now-cli"});
    auto immediate_replay = run_success(argv[2],
                                        persisted_socket,
                                        {"job",
                                         "add",
                                         "--queue-name",
                                         "source",
                                         "--type",
                                         "cli",
                                         "--now",
                                         "--command",
                                         "/bin/true",
                                         "--idempotency-key",
                                         "create-now-cli"});
    if (!immediate || !immediate_replay || *immediate != *immediate_replay || !job_id_from_summary(*immediate)) {
        return fail("symbolic-now creation did not replay the original job through the add alias");
    }

    auto recurring = run_success(argv[2],
                                 persisted_socket,
                                 {"job",
                                  "create",
                                  "--queue-name",
                                  "source",
                                  "--type",
                                  "cli",
                                  "--cron",
                                  "0 9 * * 1-5",
                                  "--timezone",
                                  "Europe/Tallinn",
                                  "--command",
                                  "/bin/true",
                                  "--attribute",
                                  "retry.max_attempts=2"});
    if (!recurring || recurring->find("cron=0 9 * * 1-5, timezone=Europe/Tallinn") == std::string::npos) {
        return fail("recurring creation did not return its cron schedule and timezone");
    }
    auto const recurring_id = job_id_from_summary(*recurring);
    auto       recurring_updated =
        recurring_id
            ? run_success(
                  argv[2],
                  persisted_socket,
                  {"job", "update", *recurring_id, "--revision", "1", "--cron", "0 10 * * 1-5", "--timezone", "UTC"})
            : std::nullopt;
    if (!recurring_updated || recurring_updated->find("cron=0 10 * * 1-5, timezone=UTC") == std::string::npos) {
        return fail("recurring update did not replace its cron schedule and timezone");
    }

    auto const defaults_path = directory.path() / "defaults.json";
    {
        auto file = std::ofstream{defaults_path};
        if (!file) {
            return fail("unable to create the queue defaults fixture");
        }
        file << R"({"retry.max_attempts":2})";
    }
    auto configured      = run_success(argv[2],
                                       persisted_socket,
                                       {"queue",
                                        "create",
                                        "configured",
                                        "--defaults-file",
                                        defaults_path.string(),
                                        "--history-retention-seconds",
                                        "0",
                                        "--runnable-wait-warning-ms",
                                        "0",
                                        "--json"});
    auto configured_json = jb::core::parse_json(configured.value_or(""));
    if (!configured_json ||
        configured_json->as_object().at("defaults").as_object().at("retry.max_attempts").as_uint() != 2 ||
        configured_json->as_object().at("history_retention_seconds").as_uint() != 0 ||
        configured_json->as_object().at("runnable_wait_warning_ms").as_uint() != 0) {
        return fail("queue configuration options did not reach the durable queue");
    }

    auto inherited      = run_success(argv[2],
                                      persisted_socket,
                                      {"job",
                                       "create",
                                       "--queue-name",
                                       "configured",
                                       "--type",
                                       "cli",
                                       "--at",
                                       cli_at,
                                       "--command",
                                       "/bin/true",
                                       "--json"});
    auto inherited_json = jb::core::parse_json(inherited.value_or(""));
    if (!inherited_json ||
        inherited_json->as_object().at("attributes").as_object().at("retry.max_attempts").as_uint() != 2) {
        return fail("new job did not inherit the configured queue default");
    }

    {
        auto file = std::ofstream{defaults_path, std::ios::trunc};
        if (!file) {
            return fail("unable to clear the queue defaults fixture");
        }
        file << "{}";
    }
    auto cleared      = run_success(argv[2],
                                    persisted_socket,
                                    {"queue",
                                     "update",
                                     "--name",
                                     "configured",
                                     "--defaults-file",
                                     defaults_path.string(),
                                     "--inherit-history-retention",
                                     "--json"});
    auto cleared_json = jb::core::parse_json(cleared.value_or(""));
    if (!cleared_json || !cleared_json->as_object().at("defaults").as_object().empty() ||
        !cleared_json->as_object().at("history_retention_seconds").is_null()) {
        return fail("queue update did not clear defaults and restore retention inheritance");
    }
    if (!verify_run_attempt_cli(argv[2], persisted_socket, directory.path(), ::geteuid() != 0 || allow_root_cli)) {
        return fail("run and attempt CLI did not preserve durable history and output");
    }
    persisted->terminate();

    if (std::filesystem::exists(marker_path) || http_sentinel->received_connection()) {
        return fail("a runner payload executed during a daemon restart");
    }
    auto const cleanup_error = directory.cleanup();
    if (cleanup_error) {
        return fail("temporary directory cleanup failed");
    }
    return EXIT_SUCCESS;
}
