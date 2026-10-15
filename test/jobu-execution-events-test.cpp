#include "execution_events.hpp"

#include "execution_diagnostics_priv.hpp"
#include "job.hpp"
#include "json.hpp"
#include "retry_policy_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "time_source.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace std::chrono_literals;

namespace {

using jb::jobu::detail::ExecutionDiagnostics;
using jb::jobu::detail::project_execution_diagnostics;

// The private policy and public event must refer to the same promoted enum, not parallel definitions.
static_assert(std::is_same_v<decltype(jb::jobu::detail::RetryPolicy::mode), RetryMode>);
static_assert(std::is_same_v<decltype(jb::jobu::detail::RetrySchedule::mode), RetryMode>);
static_assert(std::is_same_v<decltype(RetryScheduled::mode), RetryMode>);

auto id(std::uint8_t value) -> Uuid
{
    auto bytes   = Uuid::Storage{};
    bytes.back() = static_cast<std::byte>(value);
    return Uuid{bytes};
}

template <typename Event>
void check_context_copy()
{
    auto source      = Event{};
    source.execution = {.queue_id = id(1),
                        .job_id   = id(2),
                        .run_id   = id(3),
                        .type     = JobType::Http,
                        .origin   = RunOrigin::Manual};
    auto retained    = source;

    // Reusing the producer's value must not change a receiver's retained diagnostic.
    source.execution = {};
    CHECK(retained.execution.queue_id == id(1));
    CHECK(retained.execution.job_id == id(2));
    CHECK(retained.execution.run_id == id(3));
    CHECK(retained.execution.type == JobType::Http);
    CHECK(retained.execution.origin == RunOrigin::Manual);
}

auto result_field(std::string_view name, JsonValue value) -> JsonValue
{
    return {.data = JsonValue::Object{{std::string{name}, std::move(value)}}};
}

void check_empty(ExecutionDiagnostics const& projected)
{
    CHECK_FALSE(projected.exit_code);
    CHECK_FALSE(projected.signal_number);
    CHECK_FALSE(projected.http_status);
}

auto integer(std::uint64_t value, bool unsigned_representation) -> JsonValue
{
    if (unsigned_representation) {
        return {.data = value};
    }
    return {.data = static_cast<std::int64_t>(value)};
}

} // namespace

TEST_CASE("Execution event copies retain their run context and optional diagnostics", "[jobu][execution][events]")
{
    check_context_copy<AttemptStarted>();
    check_context_copy<AttemptFinished>();
    check_context_copy<RetryScheduled>();
    check_context_copy<RunFinished>();

    auto retained = std::optional<AttemptFinished>{};
    {
        auto source = AttemptFinished{
            .execution      = {.queue_id = id(1),
                               .job_id   = id(2),
                               .run_id   = id(3),
                               .type     = JobType::Cli,
                               .origin   = RunOrigin::Submitted},
            .attempt_number = 7,
            .outcome        = AttemptOutcome::Succeeded,
            .start_phase    = AttemptStartPhase::Accepted,
            .completed_at   = UtcTimePoint{20s},
            .elapsed        = 15ms,
            .exit_code      = 7,
        };
        retained = source;
        source   = {};
    }
    REQUIRE(retained);
    CHECK(retained->attempt_number == 7);
    CHECK(retained->outcome == AttemptOutcome::Succeeded);
    CHECK(retained->start_phase == AttemptStartPhase::Accepted);
    CHECK(retained->completed_at == UtcTimePoint{20s});
    CHECK(retained->elapsed == 15ms);
    CHECK(retained->exit_code == 7);
    CHECK_FALSE(retained->signal_number);
    CHECK_FALSE(retained->http_status);
}

TEST_CASE("Execution diagnostics project exact CLI exit integers within 0 through 255", "[jobu][execution][projection]")
{
    for (bool const unsigned_representation : {false, true}) {
        for (auto const code : {0U, 7U, 255U}) {
            auto projected =
                project_execution_diagnostics(JobType::Cli,
                                              result_field("exit_code", integer(code, unsigned_representation)));
            CHECK(projected.exit_code == static_cast<int>(code));
            CHECK_FALSE(projected.signal_number);
            CHECK_FALSE(projected.http_status);
        }
    }

    for (auto const& invalid : std::vector<JsonValue>{
             {.data = std::int64_t{-1}},
             {.data = std::int64_t{256}},
             {.data = std::uint64_t{256}},
             {.data = std::numeric_limits<std::int64_t>::min()},
             {.data = std::numeric_limits<std::int64_t>::max()},
             {.data = std::numeric_limits<std::uint64_t>::max()},
             {.data = 0.0},
             {.data = 255.0},
             {.data = false},
             {.data = true},
             {.data = std::string{"7"}},
             {},
             {.data = JsonValue::Array{}},
             {.data = JsonValue::Object{}},
         }) {
        check_empty(project_execution_diagnostics(JobType::Cli, result_field("exit_code", invalid)));
    }
}

TEST_CASE("Execution diagnostics limit CLI signals to positive platform signal integers",
          "[jobu][execution][projection]")
{
    for (bool const unsigned_representation : {false, true}) {
        for (int const signal : {1, SIGTERM, NSIG - 1}) {
            auto projected = project_execution_diagnostics(
                JobType::Cli,
                result_field("signal", integer(static_cast<std::uint64_t>(signal), unsigned_representation)));
            CHECK(projected.signal_number == signal);
            CHECK_FALSE(projected.exit_code);
            CHECK_FALSE(projected.http_status);
        }
    }

    for (auto const& invalid : std::vector<JsonValue>{
             {.data = std::int64_t{-1}},
             {.data = std::int64_t{0}},
             {.data = std::uint64_t{0}},
             {.data = static_cast<std::int64_t>(NSIG)},
             {.data = static_cast<std::uint64_t>(NSIG)},
             {.data = static_cast<std::int64_t>(std::numeric_limits<int>::max())},
             {.data = std::numeric_limits<std::uint64_t>::max()},
             {.data = 1.0},
             {.data = true},
             {.data = std::string{"15"}},
         }) {
        check_empty(project_execution_diagnostics(JobType::Cli, result_field("signal", invalid)));
    }
}

TEST_CASE("Execution diagnostics limit HTTP status to exact integers within 100 through 599",
          "[jobu][execution][projection]")
{
    for (bool const unsigned_representation : {false, true}) {
        for (auto const status : {100U, 204U, 503U, 599U}) {
            auto projected =
                project_execution_diagnostics(JobType::Http,
                                              result_field("status", integer(status, unsigned_representation)));
            CHECK(projected.http_status == status);
            CHECK_FALSE(projected.exit_code);
            CHECK_FALSE(projected.signal_number);
        }
    }

    for (auto const& invalid : std::vector<JsonValue>{
             {.data = std::int64_t{-1}},
             {.data = std::int64_t{99}},
             {.data = std::uint64_t{99}},
             {.data = std::int64_t{600}},
             {.data = std::uint64_t{600}},
             {.data = std::numeric_limits<std::int64_t>::max()},
             {.data = std::numeric_limits<std::uint64_t>::max()},
             {.data = 200.0},
             {.data = true},
             {.data = std::string{"200"}},
         }) {
        check_empty(project_execution_diagnostics(JobType::Http, result_field("status", invalid)));
    }
}

TEST_CASE("Execution diagnostics ignore non-object roots and nested or misspelled fields",
          "[jobu][execution][projection]")
{
    auto const nested = JsonValue{
        .data = JsonValue::Object{
                                  {"result", result_field("exit_code", {.data = std::uint64_t{7}})},
                                  {"http_status", {.data = std::uint64_t{200}}},
                                  {"signal_number", {.data = std::uint64_t{SIGTERM}}},
                                  {"Exit_code", {.data = std::uint64_t{7}}},
                                  }
    };
    for (auto const type : {JobType::Cli, JobType::Http, static_cast<JobType>(255)}) {
        for (auto const& root : std::vector<JsonValue>{
                 {},
                 {.data = false},
                 {.data = std::int64_t{7}},
                 {.data = std::uint64_t{200}},
                 {.data = 200.0},
                 {.data = std::string{"secret-body"}},
                 {.data = JsonValue::Array{result_field("status", {.data = std::uint64_t{200}})}},
                 {.data = JsonValue::Object{}},
                 nested,
             }) {
            check_empty(project_execution_diagnostics(type, root));
        }
    }
}

TEST_CASE("Execution projection uses the run type and omits hostile fields without modifying input",
          "[jobu][execution][projection]")
{
    auto const canary = std::string{"private-command-env-url-output\n\x1b[31m\xff"};
    auto const result = JsonValue{
        .data = JsonValue::Object{
                                  {"exit_code", {.data = std::uint64_t{7}}},
                                  {"signal", {.data = static_cast<std::uint64_t>(SIGTERM)}},
                                  {"status", {.data = std::uint64_t{503}}},
                                  {"type", {.data = std::string{"untrusted-runner"}}},
                                  {"outcome", {.data = canary}},
                                  {"error_code", {.data = canary}},
                                  {"message", {.data = canary}},
                                  {"payload", result_field("status", {.data = std::uint64_t{201}})},
                                  {"output", {.data = canary}},
                                  }
    };
    // Own an independent snapshot so the equality check can detect changes to any result field.
    auto const original = JsonValue{.data = result.data};

    auto cli = project_execution_diagnostics(JobType::Cli, result);
    CHECK(cli.exit_code == 7);
    CHECK(cli.signal_number == SIGTERM);
    CHECK_FALSE(cli.http_status);

    auto http = project_execution_diagnostics(JobType::Http, result);
    CHECK(http.http_status == 503U);
    CHECK_FALSE(http.exit_code);
    CHECK_FALSE(http.signal_number);
    check_empty(project_execution_diagnostics(static_cast<JobType>(255), result));
    CHECK(result == original);

    // One invalid optional diagnostic must not hide a separate valid one or reject the completion.
    auto mixed = JsonValue{
        .data = JsonValue::Object{
                                  {"exit_code", {.data = std::int64_t{-1}}},
                                  {"signal", {.data = static_cast<std::uint64_t>(SIGTERM)}},
                                  }
    };
    auto projected = project_execution_diagnostics(JobType::Cli, mixed);
    CHECK_FALSE(projected.exit_code);
    CHECK(projected.signal_number == SIGTERM);

    // Accepted cancellation replaces the original result before projection; it has no observed numeric terminal data.
    auto cancelled = result_field("outcome", {.data = std::string{"cancelled"}});
    check_empty(project_execution_diagnostics(JobType::Cli, cancelled));
    check_empty(project_execution_diagnostics(JobType::Http, cancelled));
}
