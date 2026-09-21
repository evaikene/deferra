#include "shutdown_fixture.hpp"

#include "attempt_repository_priv.hpp"
#include "cron.hpp"
#include "json.hpp"
#include "management.hpp"
#include "run_repository_priv.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace jb::test {
using namespace core;
using namespace jobu;
using namespace jobu::detail;
using namespace std::chrono_literals;

namespace {
auto observer(std::filesystem::path const& path) -> std::unique_ptr<sqlite3, decltype(&sqlite3_close)>
{
    sqlite3*   raw{};
    auto const result     = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    auto       connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{raw, sqlite3_close};
    REQUIRE(result == SQLITE_OK);
    REQUIRE(sqlite3_busy_timeout(raw, 100) == SQLITE_OK);
    return connection;
}
} // namespace

void require_shutdown_execution_environment()
{
    auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
    if (::geteuid() == 0 && (!enabled || std::string_view{enabled} != "1")) {
        SKIP("root target execution requires JOBU_TEST_ALLOW_ROOT_CLI=1 in an isolated test environment");
    }
}

ShutdownWork::ShutdownWork(std::function<std::unique_ptr<db::Driver>(std::unique_ptr<db::Driver>)> wrap)
    : storage{std::move(wrap)}
    , _report{storage.directory.path() / "group"}
{
    REQUIRE(::mkfifo(_report.c_str(), 0600) == 0);
    _report_fd = ::open(_report.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    REQUIRE(_report_fd >= 0);
    server.enqueue_response({});
}

ShutdownWork::~ShutdownWork()
{
    // Assertions inspect both identities before any destructor can supply the cleanup under test.
    // Linux watches own identity-safe fallback kills; Darwin helpers have independent emergency alarms.
    _targets = {};
    ::close(_report_fd);
}

void ShutdownWork::seed(RecoveryPolicy policy)
{
    auto queue              = recovery_queue(recovery_id(1), QueueState::Active, policy);
    queue.concurrency_limit = 2;
    storage.insert_queue(queue);
    for (std::uint32_t index = 0; index < 4; ++index) {
        auto const type                               = index % 2 == 0 ? JobType::Cli : JobType::Http;
        auto       job                                = storage.make_job(recovery_id(10 + index), queue.id, type);
        job.priority                                  = index < 2 ? 100 : 0;
        job.attributes.at("job.timeout").data         = Duration{60s};
        job.attributes.at("retry.initial_delay").data = Duration{0s};
        job.attributes.at("retry.max_delay").data     = Duration{0s};
        job.attributes.at("output.capture").data      = std::string{"always"};
        if (type == JobType::Cli) {
            job.payload.data = JsonValue::Object{
                {"command",   {.data = std::string{PROCESS_TEST_HELPER}}                                           },
                {"arguments",
                 {.data = JsonValue::Array{{.data = std::string{"daemon-group-wait"}}, {.data = _report.string()}}}}
            };
        }
        else {
            job.payload.data = JsonValue::Object{
                {"url", {.data = server.url()}}
            };
        }
        storage.insert_job(job);
        storage.insert_run(storage.make_run(recovery_id(100 + index), job));
    }
}

void ShutdownWork::until(std::function<bool()> const& predicate)
{
    auto const deadline = Clock::now() + 5s;
    while (!predicate() && Clock::now() < deadline) {
        REQUIRE(app.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
    }
    REQUIRE(predicate());
}

auto ShutdownWork::read_identities() -> bool
{
    if (identities[0] != 0) {
        return true;
    }
    auto const size = ::read(_report_fd, identities.data(), sizeof(identities));
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) {
        return false;
    }
    REQUIRE(size == sizeof(identities));
    for (std::size_t index = 0; index < identities.size(); ++index) {
        REQUIRE(identities[index] > 0);
        _targets[index] = std::make_unique<ProcessExitWatch>(identities[index]);
    }
    return true;
}

void ShutdownWork::await_work()
{
    until([&] { return read_identities() && server.requests().size() == 1; });
    REQUIRE_FALSE(server.wait_for_blocked_peer_disconnect(0ms));
    REQUIRE(count("SELECT count(*) FROM jobu_runs WHERE state='running'") == 2);
    REQUIRE(count("SELECT count(*) FROM jobu_attempts") == 2);
    REQUIRE(count("SELECT count(*) FROM jobu_runs WHERE state='scheduled'") == 2);
    REQUIRE(count("SELECT count(*) FROM jobu_attempt_output") == 0);
    for (auto const& target : _targets) {
        REQUIRE_FALSE(target->exited());
    }
}

void ShutdownWork::require_cleanup()
{
    // The response barrier is still closed; only client cancellation/teardown can close the peer.
    REQUIRE(server.wait_for_blocked_peer_disconnect(2s));
    for (auto const& target : _targets) {
        REQUIRE(target->exited(2s));
    }
    REQUIRE(server.requests().size() == 1);
    // The helper arms a 15-second emergency alarm after launch. Never accept its expiry as cleanup evidence;
    // this is a fixture watchdog bound, not a production shutdown deadline.
    REQUIRE(Clock::now() - _created_at < 15s);
}

void ShutdownWork::require_reaped() const
{
    // This must run in the runtime's process before it exits, so reparenting cannot hide an unreaped child.
    int status{};
    errno = 0;
    REQUIRE(::waitpid(identities[0], &status, WNOHANG) == -1);
    REQUIRE(errno == ECHILD);
}

auto ShutdownWork::snapshot() const -> std::vector<std::vector<std::string>>
{
    auto                                  connection = observer(storage.database_file);
    std::vector<std::vector<std::string>> rows;
    for (auto const* table : {"jobu_queues",
                              "jobu_jobs",
                              "jobu_runs",
                              "jobu_attempts",
                              "jobu_attempt_output",
                              "jobu_idempotency",
                              "jobu_secrets",
                              "jobu_secret_refs"}) {
        auto const    sql = std::string{"SELECT * FROM "} + table + " ORDER BY 1, 2, 3";
        sqlite3_stmt* raw{};
        REQUIRE(sqlite3_prepare_v2(connection.get(), sql.c_str(), -1, &raw, nullptr) == SQLITE_OK);
        auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
        int  result{};
        while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
            auto& row = rows.emplace_back();
            row.emplace_back(table);
            for (int column = 0; column < sqlite3_column_count(raw); ++column) {
                // Preserve SQLite types and embedded NUL bytes; NULL must differ from an empty value.
                auto        value = std::to_string(sqlite3_column_type(raw, column)) + ":";
                auto const* bytes = static_cast<char const*>(sqlite3_column_blob(raw, column));
                auto const  size  = sqlite3_column_bytes(raw, column);
                if (size != 0) {
                    value.append(bytes, static_cast<std::size_t>(size));
                }
                row.push_back(std::move(value));
            }
        }
        REQUIRE(result == SQLITE_DONE);
    }
    return rows;
}

auto ShutdownWork::count(std::string const& sql) const -> std::int64_t
{
    auto          connection = observer(storage.database_file);
    sqlite3_stmt* raw{};
    REQUIRE(sqlite3_prepare_v2(connection.get(), sql.c_str(), -1, &raw, nullptr) == SQLITE_OK);
    auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>{raw, sqlite3_finalize};
    REQUIRE(sqlite3_step(raw) == SQLITE_ROW);
    auto const value = sqlite3_column_int64(raw, 0);
    REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
    return value;
}

void ShutdownWork::hold_recovery()
{
    // Do this only after proving shutdown left the entire database unchanged. Pending followers remain
    // eligible throughout shutdown, but suspension prevents them and zero-delay retries racing recovery checks.
    SystemTimeSource  time;
    SystemCronEngine  cron;
    UuidV7Generator   generator{time};
    ManagementService management{storage.database, storage.registry, cron, generator, time};
    REQUIRE(management.suspend_queue(QueueSelector{recovery_id(1)}));
}

void ShutdownWork::require_recovery(bool retry)
{
    RunRepository     runs{storage.database, storage.registry};
    AttemptRepository attempts{storage.database};
    for (std::uint32_t index = 0; index < 2; ++index) {
        auto run = runs.find_by_id(recovery_id(100 + index));
        REQUIRE(run);
        REQUIRE(run->has_value());
        CHECK((**run).state == (retry ? RunState::RetryWait : RunState::Interrupted));
        auto history = attempts.list_for_run((**run).id, 10);
        REQUIRE(history);
        REQUIRE(history->size() == 1);
        auto const& attempt = history->front();
        CHECK(attempt.attempt_number == 1);
        CHECK(attempt.state == AttemptState::Completed);
        CHECK(attempt.outcome == AttemptOutcome::Interrupted);
        REQUIRE(attempt.result);
        CHECK(attempt.result->as_object().at("outcome_unknown").as_bool());
        auto output = attempts.find_output((**run).id, 1);
        REQUIRE(output);
        REQUIRE(output->has_value());
        CHECK((**output).capture_lost);
        REQUIRE((**output).stdout_bytes);
        REQUIRE((**output).stderr_bytes);
        CHECK((**output).stdout_bytes->empty());
        CHECK((**output).stderr_bytes->empty());
    }
    CHECK(count("SELECT count(*) FROM jobu_attempts") == 2);
    CHECK(count("SELECT count(*) FROM jobu_runs WHERE state='scheduled'") == 2);
    CHECK(count("SELECT count(*) FROM jobu_queues WHERE state='suspended'") == 1);
}

} // namespace jb::test
