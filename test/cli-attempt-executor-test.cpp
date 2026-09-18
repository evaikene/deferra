#include "cli/cli_attempt_executor.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "cli/process_adapter_priv.hpp"
#include "json.hpp"
#include "support/fake_process_adapter.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::cli;
using namespace jb::jobu::cli::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto json_string(std::string value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_uint(std::uint64_t value) -> JsonValue
{
    return JsonValue{.data = value};
}

auto json_array(JsonValue::Array value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_object(JsonValue::Object value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto uuid(std::string_view value) -> Uuid
{
    auto parsed = Uuid::parse(value);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto job_id() -> Uuid
{
    return uuid("11111111-1111-4111-8111-111111111111");
}

auto run_id() -> Uuid
{
    return uuid("22222222-2222-4222-8222-222222222222");
}

auto queue_id() -> Uuid
{
    return uuid("33333333-3333-4333-8333-333333333333");
}

auto string_list(std::initializer_list<std::string_view> values) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(values.size());
    for (auto value : values) {
        result.push_back({.data = std::string{value}});
    }
    return result;
}

auto materialized_attributes(AttributeSet const& overrides = {}) -> AttributeSet
{
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, overrides);
    REQUIRE(attributes);
    return std::move(attributes).value();
}

auto minimal_payload(std::string command = "/bin/test-command") -> JsonValue
{
    return json_object({
        {"command", json_string(std::move(command))}
    });
}

auto complete_payload() -> JsonValue
{
    return json_object({
        {"arguments",           json_array({json_string("first"), json_string("--second")})},
        {"command",             json_string("test-command")                                },
        {"environment",
         json_object({
             {"KEEP", json_string("literal-value")},
             {"PATH", json_string("/bin:/usr/bin")},
             {"REMOVE", JsonValue{}},
         })                                                                                },
        {"expected_exit_codes", json_array({json_uint(0), json_uint(7)})                   },
        {"working_directory",   json_string("/work")                                       },
    });
}

auto start_request(AttemptNumber       attempt_number = 1,
                   AttributeSet const& overrides      = {},
                   JsonValue           payload        = minimal_payload()) -> AttemptStartRequest
{
    return {
        .key        = {.run_id = run_id(), .attempt_number = attempt_number},
        .job_id     = job_id(),
        .queue_id   = queue_id(),
        .type       = JobType::Cli,
        .attributes = materialized_attributes(overrides),
        .payload    = std::move(payload),
        .started_at = UtcTimePoint{10s},
    };
}

auto byte_text(ByteBuffer const& value) -> std::string_view
{
    return as_string_view(ByteView{value});
}

auto result_string(AttemptCompletion const& completion, std::string_view name) -> std::string const&
{
    return completion.result.as_object().at(std::string{name}).as_string();
}

struct TestApplication {
    std::array<char const*, 2> arguments{"cli-attempt-executor-test", nullptr};
    Application                application{1, arguments.data()};
};

struct ExecutorFixture {
    explicit ExecutorFixture(CliAttemptExecutorOptions options = {}, std::uint64_t effective_user_id = 1000U)
    {
        auto adapter_owner  = std::make_unique<FakeProcessAdapter>();
        adapter             = adapter_owner.get();
        observation         = adapter_owner->observation();
        auto identity_owner = std::make_unique<FakeEffectiveIdentityProbe>(effective_user_id);
        identity            = identity_owner.get();
        executor = CliAttemptExecutorTestAccess::create(options, std::move(adapter_owner), std::move(identity_owner));
    }

    TestApplication                                application;
    FakeProcessAdapter*                            adapter{nullptr};
    FakeEffectiveIdentityProbe*                    identity{nullptr};
    std::shared_ptr<FakeProcessAdapterObservation> observation;
    std::unique_ptr<CliAttemptExecutor>            executor;
};

auto exited(int code, bool stdout_lost = false, bool stderr_lost = false) -> ProcessExit
{
    return {
        .kind        = ProcessExitKind::Exited,
        .exit_code   = code,
        .stdout_lost = stdout_lost,
        .stderr_lost = stderr_lost,
    };
}

} // anonymous namespace

TEST_CASE("CLI attempt executor exposes Object ownership and dynamic availability", "[jobu][cli][executor]")
{
    SECTION("public construction remains unavailable before Process integration")
    {
        TestApplication    application;
        CliAttemptExecutor executor;

        CHECK_FALSE(executor.is_available(JobType::Cli));
        CHECK_FALSE(executor.is_available(JobType::Http));
        auto rejected = executor.start(start_request(), [](AttemptCompletion const&) {});
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobu.cli.start_failed");
        CHECK(rejected.error().detail == "core.process.monitor_unsupported");
    }

    SECTION("private fake construction follows type loop and identity policy")
    {
        ExecutorFixture fixture;

        CHECK(fixture.executor->is_available(JobType::Cli));
        CHECK_FALSE(fixture.executor->is_available(JobType::Http));
        CHECK_FALSE(fixture.executor->is_available(static_cast<JobType>(255)));
        fixture.identity->set_effective_user_id(0);
        CHECK_FALSE(fixture.executor->is_available(JobType::Cli));
    }

    SECTION("unsafe override permits an injected root identity")
    {
        ExecutorFixture fixture{{.allow_root = true}, 0};
        CHECK(fixture.executor->is_available(JobType::Cli));
        REQUIRE(fixture.executor->start(start_request(), [](AttemptCompletion const&) {}));
        REQUIRE(fixture.observation->starts.size() == 1U);
        CHECK_FALSE(fixture.observation->starts.front().start_info.require_non_root);
    }

    SECTION("Object parent owns a heap-allocated public executor")
    {
        Object parent;
        auto*  child = new CliAttemptExecutor(CliAttemptExecutorOptions{}, &parent);
        REQUIRE(parent.children().size() == 1U);
        CHECK(parent.children().front() == child);
        CHECK(child->parent() == &parent);
    }
}

TEST_CASE("CLI attempt executor requires its current owner EventLoop", "[jobu][cli][executor]")
{
    auto adapter_owner  = std::make_unique<FakeProcessAdapter>();
    auto identity_owner = std::make_unique<FakeEffectiveIdentityProbe>();
    auto executor       = CliAttemptExecutorTestAccess::create({}, std::move(adapter_owner), std::move(identity_owner));

    CHECK_FALSE(executor->is_available(JobType::Cli));
    auto result = executor->start(start_request(), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.cli.event_loop_unavailable");
}

TEST_CASE("CLI attempt executor prepares explicit process policy and metadata", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            overrides = AttributeSet{
        {"cli.retry_exit_codes",  {.data = string_list({"2-4"})} },
        {"cli.termination_grace", {.data = 3s}                   },
        {"job.timeout",           {.data = 42s}                  },
        {"output.capture",        {.data = std::string{"always"}}},
        {"output.stderr_limit",   {.data = std::int64_t{5}}      },
        {"output.stdout_limit",   {.data = std::int64_t{7}}      },
    };
    auto completions = std::vector<AttemptCompletion>{};

    REQUIRE(fixture.executor->start(start_request(1, overrides, complete_payload()), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    CHECK(completions.empty());
    REQUIRE(fixture.observation->starts.size() == 1U);

    auto const& accepted = fixture.observation->starts.front();
    CHECK(accepted.start_info.executable == "test-command");
    CHECK(accepted.start_info.arguments == std::vector<std::string>{"first", "--second"});
    CHECK(accepted.start_info.working_directory == "/work");
    CHECK(accepted.start_info.timeout == 42s);
    CHECK(accepted.start_info.termination_grace == 3s);
    CHECK(accepted.start_info.require_non_root);
#if defined(__linux__)
    CHECK(accepted.start_info.prevent_privilege_gain);
#else
    CHECK_FALSE(accepted.start_info.prevent_privilege_gain);
#endif
    CHECK(accepted.start_info.environment.size() == 5U);
    CHECK(accepted.start_info.environment.at("KEEP") == "literal-value");
    CHECK(accepted.start_info.environment.at("PATH") == "/bin:/usr/bin");
    CHECK_FALSE(accepted.start_info.environment.contains("REMOVE"));
    CHECK(accepted.start_info.environment.at("JOBU_JOB_ID") == job_id().to_string());
    CHECK(accepted.start_info.environment.at("JOBU_RUN_ID") == run_id().to_string());
    CHECK(accepted.start_info.environment.at("JOBU_ATTEMPT") == "1");
    CHECK_FALSE(accepted.start_info.environment.contains("HOME"));

    REQUIRE(fixture.adapter->emit_standard_output(accepted.id, as_bytes("0123456789")));
    REQUIRE(fixture.adapter->emit_standard_error(accepted.id, as_bytes("abcdefgh")));
    REQUIRE(fixture.adapter->finish(accepted.id, exited(7)));

    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].key == AttemptKey{.run_id = run_id(), .attempt_number = 1});
    CHECK(completions[0].outcome == AttemptOutcome::Succeeded);
    REQUIRE(completions[0].output);
    REQUIRE(completions[0].output->primary);
    REQUIRE(completions[0].output->diagnostic);
    CHECK(byte_text(completions[0].output->primary->bytes) == "0123789");
    CHECK(byte_text(completions[0].output->diagnostic->bytes) == "abcgh");
    CHECK(fixture.observation->retired == std::vector<ProcessOperationId>{accepted.id});
}

TEST_CASE("CLI attempt executor rejects invalid starts without retaining callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callback_count = std::size_t{0};
    auto            callback       = [&](AttemptCompletion const&) { ++callback_count; };

    SECTION("unsupported type")
    {
        auto request = start_request();
        request.type = JobType::Http;
        auto result  = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.unsupported_type");
    }

    SECTION("zero attempt number")
    {
        auto request               = start_request();
        request.key.attempt_number = 0;
        auto result                = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_start");
    }

    SECTION("empty completion handler")
    {
        auto result = fixture.executor->start(start_request(), {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_start");
    }

    SECTION("invalid materialized attributes")
    {
        auto request = start_request();
        request.attributes.erase("job.timeout");
        auto result = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_snapshot");
        CHECK(result.error().detail == "attributes.invalid");
    }

    SECTION("invalid durable payload")
    {
        auto request    = start_request();
        request.payload = json_object({});
        auto result     = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_snapshot");
        CHECK(result.error().detail == "missing_command");
    }

    SECTION("adapter rejection exposes only a stable process code")
    {
        fixture.adapter->set_start_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "core.process.watch_failed",
            .message  = "private-marker-message",
            .detail   = "private-marker-detail",
        });
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.start_failed");
        CHECK(result.error().detail == "core.process.watch_failed");
        CHECK(result.error().message.find("private-marker") == std::string::npos);
        CHECK(result.error().detail.find("private-marker") == std::string::npos);
    }

    SECTION("invalid adapter error code is replaced with a fixed safe detail")
    {
        fixture.adapter->set_start_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "private-marker-code",
            .message  = "private-marker-message",
        });
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().detail == "core.process.resource_setup_failed");
    }

    SECTION("zero operation identity is rejected and cleaned up")
    {
        fixture.adapter->set_next_operation_id(0);
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.start_failed");
        CHECK(result.error().detail == "core.process.invalid_state");
        CHECK(fixture.observation->shutdown == std::vector<ProcessOperationId>{0});
    }

    CHECK(callback_count == 0U);
    CHECK(fixture.adapter->pending_operation_ids().empty());
}

TEST_CASE("CLI attempt executor rejects duplicate attempt and operation identities", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callbacks = std::size_t{0};

    fixture.adapter->set_next_operation_id(9);
    REQUIRE(fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callbacks; }));

    auto duplicate_key = fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callbacks; });
    REQUIRE_FALSE(duplicate_key);
    CHECK(duplicate_key.error().code == "jobu.cli.duplicate_attempt");

    fixture.adapter->set_next_operation_id(9);
    auto duplicate_operation =
        fixture.executor->start(start_request(2), [&](AttemptCompletion const&) { ++callbacks; });
    REQUIRE_FALSE(duplicate_operation);
    CHECK(duplicate_operation.error().code == "jobu.cli.start_failed");
    CHECK(duplicate_operation.error().detail == "core.process.invalid_state");
    CHECK(callbacks == 0U);
    CHECK(fixture.adapter->pending_operation_ids() == std::vector<ProcessOperationId>{9});
}

TEST_CASE("CLI attempt executor repeats the root check immediately before adapter start", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    fixture.identity->set_sequence({1000U, 0U});

    CHECK(fixture.executor->is_available(JobType::Cli));
    auto result = fixture.executor->start(start_request(), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.cli.root_forbidden");
    CHECK(fixture.identity->call_count() == 2U);
    CHECK(fixture.observation->starts.empty());
    CHECK(fixture.adapter->pending_operation_ids().empty());
}

TEST_CASE("CLI attempt executor maps outcomes and capture modes through existing policy", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            completions = std::vector<AttemptCompletion>{};

    auto run = [&](AttemptNumber       attempt_number,
                   AttributeSet const& overrides,
                   ProcessExit         exit,
                   std::string_view    output = "stream-marker") -> AttemptCompletion const& {
        REQUIRE(fixture.executor->start(start_request(attempt_number, overrides), [&](AttemptCompletion completion) {
            completions.push_back(std::move(completion));
        }));
        auto const operation_id = fixture.observation->starts.back().id;
        REQUIRE(fixture.adapter->emit_standard_output(operation_id, as_bytes(output)));
        REQUIRE(fixture.adapter->finish(operation_id, std::move(exit)));
        return completions.back();
    };

    auto const& success = run(1, {}, exited(0));
    CHECK(success.outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(success.output);
    CHECK(result_string(success, "outcome") == "success");

    auto const& retryable = run(2, {}, exited(2));
    CHECK(retryable.outcome == AttemptOutcome::Failed);
    CHECK(retryable.failure_disposition == FailureDisposition::Retryable);
    REQUIRE(retryable.output);

    auto terminal_overrides = AttributeSet{
        {"cli.retry_exit_codes", {.data = string_list({})}},
    };
    auto const& terminal = run(3, terminal_overrides, exited(2));
    CHECK(terminal.outcome == AttemptOutcome::Failed);
    CHECK(terminal.failure_disposition == FailureDisposition::Terminal);

    auto const& signalled = run(4, {}, {.kind = ProcessExitKind::Signaled, .signal_number = 15});
    CHECK(signalled.outcome == AttemptOutcome::Failed);
    CHECK(signalled.failure_disposition == FailureDisposition::Retryable);

    auto const& timed_out = run(5, {}, {.kind = ProcessExitKind::TimedOut, .signal_number = 9});
    CHECK(timed_out.outcome == AttemptOutcome::Failed);
    CHECK(timed_out.failure_disposition == FailureDisposition::Retryable);

    auto const& cancelled = run(6, {}, {.kind = ProcessExitKind::Cancelled, .exit_code = 0});
    CHECK(cancelled.outcome == AttemptOutcome::Cancelled);
    CHECK_FALSE(cancelled.failure_disposition);

    auto const& start_failed = run(7,
                                   {
    },
                                   {
                                       .kind = ProcessExitKind::StartFailed,
                                       .start_error =
                                           Error{
                                               .category = ErrorCategory::NotFound,
                                               .code     = "core.process.exec_failed",
                                               .message  = "private-marker-message",
                                               .detail   = "private-marker-detail",
                                           },
                                   });
    CHECK(start_failed.outcome == AttemptOutcome::Failed);
    CHECK(start_failed.failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(start_failed, "error_code") == "core.process.exec_failed");
    auto serialized = serialize_json(start_failed.result);
    REQUIRE(serialized);
    CHECK(serialized->find("private-marker") == std::string::npos);
    CHECK(serialized->find("stream-marker") == std::string::npos);

    auto none_overrides = AttributeSet{
        {"output.capture", {.data = std::string{"none"}}},
    };
    auto const& discarded = run(8, none_overrides, exited(2), "discarded-output");
    CHECK_FALSE(discarded.output);
    auto const& stdout_result = discarded.result.as_object().at("stdout").as_object();
    CHECK(stdout_result.at("captured_bytes").as_uint() == 0U);
    CHECK(stdout_result.at("total_bytes").as_uint() == std::string_view{"discarded-output"}.size());
    CHECK(stdout_result.at("truncated").as_bool());

    auto const& lost = run(9, none_overrides, exited(2, true));
    CHECK(lost.result.as_object().at("capture_lost").as_bool());

    // A malformed private-adapter observation is an internal defect, but it must still discharge the accepted
    // completion exactly once with a bounded safe result.
    auto const& internal = run(10, {}, ProcessExit{});
    CHECK(internal.outcome == AttemptOutcome::Failed);
    CHECK(internal.failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(internal, "outcome") == "internal_error");
    CHECK(result_string(internal, "error_code") == "jobu.cli.invalid_result");
}

TEST_CASE("CLI attempt executor retains completion through cancellation", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            completions = std::vector<AttemptCompletion>{};
    auto const      key         = start_request().key;

    auto missing = fixture.executor->cancel(key);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "jobu.cli.attempt_not_found");

    REQUIRE(fixture.executor->start(start_request(), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    auto const operation_id = fixture.observation->starts.front().id;

    fixture.adapter->set_stop_error(Error{
        .category = ErrorCategory::Io,
        .code     = "core.process.signal_failed",
        .message  = "private-marker-message",
        .detail   = "private-marker-detail",
    });
    auto rejected = fixture.executor->cancel(key);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.cli.cancel_failed");
    CHECK(rejected.error().detail == "core.process.signal_failed");
    CHECK(completions.empty());

    fixture.adapter->set_stop_error(std::nullopt);
    REQUIRE(fixture.executor->cancel(key));
    REQUIRE(fixture.executor->cancel(key));
    CHECK(completions.empty());
    REQUIRE(fixture.observation->stops.size() == 3U);
    CHECK(std::ranges::all_of(fixture.observation->stops,
                              [](auto const& stop) { return stop.reason == ProcessStopReason::Cancelled; }));

    REQUIRE(fixture.adapter->finish(operation_id, {.kind = ProcessExitKind::Cancelled, .signal_number = 15}));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].outcome == AttemptOutcome::Cancelled);
}

TEST_CASE("CLI attempt executor fences stale events and retires state before callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            overrides = AttributeSet{
        {"output.capture", {.data = std::string{"always"}}},
    };
    auto completions      = std::vector<AttemptCompletion>{};
    auto reentrant_cancel = std::optional<Result<void, Error>>{};
    auto reentrant_start  = std::optional<Result<void, Error>>{};

    REQUIRE(fixture.executor->start(start_request(1, overrides), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
        reentrant_cancel.emplace(fixture.executor->cancel(start_request().key));
        reentrant_start.emplace(fixture.executor->start(start_request(1, overrides), [&](AttemptCompletion second) {
            completions.push_back(std::move(second));
        }));
    }));
    auto const first_id = fixture.observation->starts.front().id;
    auto       stale    = fixture.adapter->snapshot_sink(first_id);
    REQUIRE(stale);

    REQUIRE(fixture.adapter->emit_standard_output(first_id, as_bytes("wrong"), first_id + 100U));
    REQUIRE(fixture.adapter->finish(first_id, exited(0), first_id + 100U));
    CHECK(completions.empty());

    REQUIRE(fixture.adapter->emit_standard_output(first_id, as_bytes("first")));
    REQUIRE(fixture.adapter->finish(first_id, exited(0)));
    REQUIRE(completions.size() == 1U);
    REQUIRE(reentrant_cancel);
    REQUIRE_FALSE(*reentrant_cancel);
    CHECK(reentrant_cancel->error().code == "jobu.cli.attempt_not_found");
    REQUIRE(reentrant_start);
    REQUIRE(*reentrant_start);
    REQUIRE(fixture.observation->starts.size() == 2U);
    auto const second_id = fixture.observation->starts.back().id;

    stale->standard_output(first_id, as_bytes("stale"));
    stale->finished(first_id, exited(0));
    CHECK(completions.size() == 1U);

    REQUIRE(fixture.adapter->emit_standard_output(second_id, as_bytes("second")));
    REQUIRE(fixture.adapter->finish(second_id, exited(0)));
    REQUIRE(completions.size() == 2U);
    REQUIRE(completions[1].output);
    REQUIRE(completions[1].output->primary);
    CHECK(byte_text(completions[1].output->primary->bytes) == "second");
}

TEST_CASE("CLI attempt executor destruction cleans active operations and suppresses callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callback_count = std::size_t{0};
    REQUIRE(fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callback_count; }));
    REQUIRE(fixture.executor->start(start_request(2), [&](AttemptCompletion const&) { ++callback_count; }));

    auto const first_id  = fixture.observation->starts[0].id;
    auto const second_id = fixture.observation->starts[1].id;
    auto       stale     = fixture.adapter->snapshot_sink(first_id);
    REQUIRE(stale);

    fixture.executor.reset();
    CHECK(callback_count == 0U);
    auto shutdown = fixture.observation->shutdown;
    std::ranges::sort(shutdown);
    CHECK(shutdown == std::vector<ProcessOperationId>{first_id, second_id});

    stale->standard_output(first_id, as_bytes("late-output"));
    stale->finished(first_id, exited(0));
    CHECK(callback_count == 0U);
}
