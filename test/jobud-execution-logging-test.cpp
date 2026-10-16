#include "execution_logging_priv.hpp"

#include "daemon_logger_priv.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/operational_log_capture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal constants.
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobud::detail;
using namespace jb::test;
using namespace std::chrono_literals;

auto execution(JobType type = JobType::Cli, RunOrigin origin = RunOrigin::Scheduled) -> ExecutionContext
{
    auto queue = Uuid::parse("00000000-0000-7000-8000-000000000001");
    auto job   = Uuid::parse("00000000-0000-7000-8000-000000000002");
    auto run   = Uuid::parse("00000000-0000-7000-8000-000000000003");
    REQUIRE(queue);
    REQUIRE(job);
    REQUIRE(run);
    return {.queue_id = *queue, .job_id = *job, .run_id = *run, .type = type, .origin = origin};
}

void emit_execution_examples(ExecutionContext const& context, RetryMode mode = RetryMode::Reschedule)
{
    auto const time = UtcTimePoint{1234567us};
    log_attempt_started({.execution = context, .attempt_number = 2, .started_at = time});
    auto finished = AttemptFinished{.execution      = context,
                                    .attempt_number = 2,
                                    .outcome        = AttemptOutcome::Succeeded,
                                    .start_phase    = AttemptStartPhase::Accepted,
                                    .completed_at   = time,
                                    .elapsed        = 0ms};
    if (context.type == JobType::Cli) {
        finished.exit_code = 37;
    }
    else {
        finished.http_status = 404;
    }
    log_attempt_finished(finished);
    finished.outcome = AttemptOutcome::Failed;
    log_attempt_finished(finished);
    finished.outcome     = AttemptOutcome::Cancelled;
    finished.exit_code   = {};
    finished.http_status = {};
    log_attempt_finished(finished);
    log_retry_scheduled(
        {.execution = context, .completed_attempt = 2, .next_attempt = 3, .runnable_at = time, .mode = mode});
    log_run_finished(
        {.execution = context, .state = RunState::Succeeded, .completed_at = time, .completing_attempt = 2});
    log_run_finished({.execution = context, .state = RunState::Failed, .completed_at = time, .completing_attempt = 2});
    log_run_finished({.execution = context, .state = RunState::Cancelled, .completed_at = time});
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};

/// An anonymous file avoids pipe-capacity deadlocks. Restore stderr before any assertions.
class StderrCapture final {
public:
    StderrCapture()
        : _file{std::tmpfile()}
    {
        REQUIRE(_file);
        std::fflush(stderr);
        _saved = ::dup(STDERR_FILENO);
        REQUIRE(_saved >= 0);
        REQUIRE(::dup2(::fileno(_file.get()), STDERR_FILENO) >= 0);
    }

    ~StderrCapture() { restore(); }

    StderrCapture(StderrCapture const&)                    = delete;
    auto operator=(StderrCapture const&) -> StderrCapture& = delete;

    auto text() -> std::string
    {
        restore();
        REQUIRE(std::fseek(_file.get(), 0, SEEK_SET) == 0);
        std::string            result;
        std::array<char, 4096> buffer{};
        while (auto count = std::fread(buffer.data(), 1, buffer.size(), _file.get())) {
            result.append(buffer.data(), count);
        }
        return result;
    }

private:
    void restore()
    {
        if (_saved >= 0) {
            std::fflush(stderr);
            if (::dup2(_saved, STDERR_FILENO) < 0) {
                std::terminate();
            }
            ::close(_saved);
            _saved = -1;
            std::clearerr(stderr);
        }
    }

    std::unique_ptr<std::FILE, FileCloser> _file;
    int                                    _saved{-1};
};

auto parsed(std::string_view text) -> JsonValue
{
    auto value = parse_json(text);
    REQUIRE(value);
    return std::move(*value);
}

auto encoded(JsonValue const& value) -> std::string
{
    auto text = serialize_json(value);
    REQUIRE(text);
    return std::move(*text);
}

auto record_lines(std::string_view output, LoggingFormat format) -> std::vector<JsonValue>
{
    std::vector<JsonValue> records;
    while (!output.empty()) {
        auto const end = output.find('\n');
        REQUIRE(end != std::string_view::npos);
        auto const line = output.substr(0, end);
        if (format == LoggingFormat::Json) {
            records.push_back(parsed(line));
        }
        else {
            // Decode the text sink's quoted members independently of its time/thread prefix.
            auto const level   = line.find(" [");
            auto const thread  = line.find("] thread=");
            auto const event   = line.find(" event=");
            auto const message = line.find(" message=");
            auto const fields  = line.find(" fields=");
            REQUIRE(level < thread);
            REQUIRE(thread < event);
            REQUIRE(event < message);
            REQUIRE(message < fields);
            records.push_back({
                .data = JsonValue::Object{
                                          {"level", {.data = std::string{line.substr(level + 2, thread - level - 2)}}},
                                          {"event", parsed(line.substr(event + 7, message - event - 7))},
                                          {"message", parsed(line.substr(message + 9, fields - message - 9))},
                                          {"fields", parsed(line.substr(fields + 8))},
                                          }
            });
        }
        output.remove_prefix(end + 1);
    }
    return records;
}

} // namespace

TEST_CASE("Execution formatters preserve owning identity and safe typed diagnostics", "[jobud][logging][execution]")
{
    auto const          type    = GENERATE(JobType::Cli, JobType::Http);
    auto const          origin  = GENERATE(RunOrigin::Scheduled, RunOrigin::Manual, RunOrigin::Submitted);
    auto const          mode    = GENERATE(RetryMode::Blocking, RetryMode::Reschedule);
    auto const          context = execution(type, origin);
    OperationalLogGuard logs;
    emit_execution_examples(context, mode);
    auto const           records = logs.capture->records();
    constexpr std::array names{"jobud.attempt.started",
                               "jobud.attempt.succeeded",
                               "jobud.attempt.failed",
                               "jobud.attempt.cancelled",
                               "jobud.run.retry_scheduled",
                               "jobud.run.succeeded",
                               "jobud.run.failed",
                               "jobud.run.cancelled"};
    REQUIRE(records.size() == names.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto const& record = records[index];
        CHECK(record.event == names[index]);
        CHECK(record.level == (index == 6 ? LogLevel::Warning : LogLevel::Debug1));
        CHECK(record.fields.at("queue_id").as_string() == context.queue_id.to_string());
        CHECK(record.fields.at("job_id").as_string() == context.job_id.to_string());
        CHECK(record.fields.at("run_id").as_string() == context.run_id.to_string());
        CHECK(record.fields.at("type").as_string() == (type == JobType::Cli ? "cli" : "http"));
        std::string_view origin_name = "scheduled";
        if (origin == RunOrigin::Manual) {
            origin_name = "manual";
        }
        else if (origin == RunOrigin::Submitted) {
            origin_name = "submitted";
        }
        CHECK(record.fields.at("origin").as_string() == origin_name);
        auto const* time_field = "completed_at";
        if (index == 0) {
            time_field = "started_at";
        }
        else if (index == 4) {
            time_field = "runnable_at";
        }
        CHECK(record.fields.at(time_field).as_string() == "1970-01-01T00:00:01.234567Z");
        CHECK_FALSE(record.fields.contains("name"));
        CHECK_FALSE(record.fields.contains("payload"));
        CHECK_FALSE(record.fields.contains("result"));
    }

    CHECK(records[0].message == (type == JobType::Cli ? "CLI attempt 2 started." : "HTTP attempt 2 started."));
    CHECK(records[0].fields.at("attempt_number").as_uint() == 2);
    CHECK(records[1].message == (type == JobType::Cli ? "CLI attempt 2 succeeded with exit code 37."
                                                      : "HTTP attempt 2 succeeded with HTTP status 404."));
    CHECK(records[1].fields.at("elapsed_ms").as_int() == 0);
    CHECK_FALSE(records[1].fields.contains("start_phase"));
    if (type == JobType::Cli) {
        CHECK(records[1].fields.at("exit_code").as_int() == 37);
        CHECK_FALSE(records[1].fields.contains("http_status"));
    }
    else {
        CHECK(records[1].fields.at("http_status").as_uint() == 404);
        CHECK_FALSE(records[1].fields.contains("exit_code"));
    }
    CHECK(records[2].fields.at("start_phase").as_string() == "accepted");
    CHECK(records[4].fields.at("completed_attempt").as_uint() == 2);
    CHECK(records[4].fields.at("next_attempt").as_uint() == 3);
    CHECK(records[4].fields.at("retry_mode").as_string() == (mode == RetryMode::Blocking ? "blocking" : "reschedule"));
    CHECK(records[4].message == "Run retry scheduled for 1970-01-01T00:00:01.234567Z.");
    CHECK(records[5].fields.at("completing_attempt").as_uint() == 2);
    CHECK(records[6].fields.at("completing_attempt").as_uint() == 2);
    CHECK_FALSE(records[7].fields.contains("completing_attempt"));
}

TEST_CASE("Execution formatting omits unavailable diagnostics and describes start failures",
          "[jobud][logging][execution]")
{
    auto const          phase = GENERATE(AttemptStartPhase::Accepted,
                                         AttemptStartPhase::PreparationFailed,
                                         AttemptStartPhase::ExecutorRejected);
    OperationalLogGuard logs;
    log_attempt_finished({.execution      = execution(),
                          .attempt_number = 1,
                          .outcome        = AttemptOutcome::Failed,
                          .start_phase    = phase,
                          .completed_at   = UtcTimePoint{}});
    auto const records = logs.capture->records();
    REQUIRE(records.size() == 1);
    auto const& record = records.front();
    CHECK(record.fields.size() == 8);
    CHECK_FALSE(record.fields.contains("elapsed_ms"));
    CHECK_FALSE(record.fields.contains("exit_code"));
    CHECK_FALSE(record.fields.contains("signal_number"));
    CHECK_FALSE(record.fields.contains("http_status"));
    if (phase == AttemptStartPhase::PreparationFailed) {
        CHECK(record.fields.at("start_phase").as_string() == "preparation_failed");
        CHECK(record.message == "CLI attempt 1 failed during preparation.");
    }
    else if (phase == AttemptStartPhase::ExecutorRejected) {
        CHECK(record.fields.at("start_phase").as_string() == "executor_rejected");
        CHECK(record.message == "CLI attempt 1 failed because the executor rejected it.");
    }
    else {
        CHECK(record.message == "CLI attempt 1 failed.");
    }
}

TEST_CASE("Execution formatting retains signal numbers without inventing live recovery events",
          "[jobud][logging][execution]")
{
    OperationalLogGuard logs;
    log_attempt_finished({.execution      = execution(),
                          .attempt_number = 1,
                          .outcome        = AttemptOutcome::Failed,
                          .start_phase    = AttemptStartPhase::Accepted,
                          .completed_at   = UtcTimePoint{},
                          .signal_number  = SIGTERM});
    log_attempt_finished({.execution      = execution(),
                          .attempt_number = 1,
                          .outcome        = AttemptOutcome::Interrupted,
                          .start_phase    = AttemptStartPhase::Accepted,
                          .completed_at   = UtcTimePoint{}});
    for (auto state : {RunState::Scheduled, RunState::Running, RunState::RetryWait, RunState::Interrupted}) {
        log_run_finished({.execution = execution(), .state = state, .completed_at = UtcTimePoint{}});
    }
    auto const records = logs.capture->records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].fields.at("signal_number").as_int() == SIGTERM);
    CHECK(records[0].message == "CLI attempt 1 failed with signal " + std::to_string(SIGTERM) + '.');
}

TEST_CASE("Execution sinks preserve messages fields and exact level names at every threshold",
          "[jobud][logging][execution]")
{
    auto const format = GENERATE(LoggingFormat::Json, LoggingFormat::Text);
    auto const level  = GENERATE(LogLevel::Error,
                                 LogLevel::Warning,
                                 LogLevel::Info,
                                 LogLevel::Debug1,
                                 LogLevel::Debug2,
                                 LogLevel::Debug3);
    auto       emit   = [] {
        emit_execution_examples(execution());
        log_event_message(LogLevel::Error, "test.error", "Infrastructure failure.");
        log_event_message(LogLevel::Info, "test.info", "Inventory loaded.");
        log_event_message(LogLevel::Debug2, "test.debug2", "Empty maintenance.");
        log_event_message(LogLevel::Debug3, "test.debug3", "Additional detail.");
    };
    OperationalLogGuard logs;
    emit();
    auto const expected = logs.capture->records();

    StderrCapture capture;
    {
        DaemonLogScope sink{format, level};
        emit();
    }
    auto const  output  = capture.text();
    auto const  records = record_lines(output, format);
    std::size_t index{0};
    for (auto const& reference : expected) {
        if (reference.level > level) {
            continue;
        }
        REQUIRE(index < records.size());
        auto const&          record = records[index++].as_object();
        // These are wire spellings, independently asserted rather than derived from log_level_name().
        constexpr std::array names{"FATAL", "ERROR", "WARN", "INFO", "DBG1", "DBG2", "DBG3"};
        CHECK(record.at("level").as_string() == names.at(static_cast<std::size_t>(reference.level)));
        CHECK(record.at("event").as_string() == reference.event);
        CHECK(record.at("message").as_string() == reference.message);
        CHECK(encoded(record.at("fields")) == encoded({.data = reference.fields}));
    }
    CHECK(index == records.size());
    CHECK(output.find("DEBUG1") == std::string::npos);
    CHECK(output.find("WARNING") == std::string::npos);
}
