#include "support/fake_attempt_executor.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaximumResultBytes = std::size_t{256} * 1024U;

template <typename T>
auto make_json(T value) -> jb::core::JsonValue
{
    return {.data = std::move(value)};
}

auto object_with_text(std::string text) -> jb::core::JsonValue
{
    auto object = jb::core::JsonValue::Object{};
    object.emplace("value", make_json(std::move(text)));
    return make_json(std::move(object));
}

auto attempt_key(std::string_view run_id, jb::jobu::AttemptNumber attempt_number = 1) -> jb::jobu::AttemptKey
{
    return {
        .run_id         = *Uuid::parse(run_id),
        .attempt_number = attempt_number,
    };
}

auto start_request(jb::jobu::AttemptKey key, jb::jobu::JobType type = jb::jobu::JobType::Cli)
    -> jb::jobu::AttemptStartRequest
{
    return {
        .key        = key,
        .job_id     = *Uuid::parse("00000000-0000-0000-0000-000000000010"),
        .queue_id   = *Uuid::parse("00000000-0000-0000-0000-000000000020"),
        .type       = type,
        .attributes = {},
        .payload    = object_with_text("payload"),
        .started_at = UtcTimePoint{100s},
    };
}

auto attempt_completion(jb::jobu::AttemptKey     key,
                        jb::jobu::AttemptOutcome outcome = jb::jobu::AttemptOutcome::Succeeded)
    -> jb::jobu::AttemptCompletion
{
    return {
        .key                 = key,
        .outcome             = outcome,
        .failure_disposition = std::nullopt,
        .retry_not_before    = std::nullopt,
        .result              = object_with_text("result"),
    };
}

} // anonymous namespace

TEST_CASE("FakeTimeSource advances clocks together and permits independent jumps", "[test][time]")
{
    jb::test::FakeTimeSource time_source;
    time_source.set_utc(UtcTimePoint{100s});
    time_source.set_monotonic(TimePoint{10s});

    time_source.advance(5s);
    CHECK(time_source.utc_now() == UtcTimePoint{105s});
    CHECK(time_source.monotonic_now() == TimePoint{15s});

    time_source.set_utc(UtcTimePoint{90s});
    CHECK(time_source.utc_now() == UtcTimePoint{90s});
    CHECK(time_source.monotonic_now() == TimePoint{15s});
}

TEST_CASE("TemporaryDirectory creates unique directories and removes them", "[test][filesystem]")
{
    std::filesystem::path first_path;
    {
        jb::test::TemporaryDirectory first;
        jb::test::TemporaryDirectory second;
        first_path = first.path();

        CHECK(std::filesystem::exists(first.path()));
        CHECK(std::filesystem::exists(second.path()));
        CHECK(first.path() != second.path());
        CHECK_FALSE(first.cleanup());
        CHECK_FALSE(std::filesystem::exists(first_path));
    }
    CHECK_FALSE(std::filesystem::exists(first_path));
}

TEST_CASE("TemporaryDirectory release transfers cleanup ownership", "[test][filesystem]")
{
    std::filesystem::path released_path;
    {
        jb::test::TemporaryDirectory directory;
        released_path = directory.release();
        CHECK(std::filesystem::exists(released_path));
    }
    CHECK(std::filesystem::exists(released_path));
    CHECK(std::filesystem::remove_all(released_path) > 0);
}

TEST_CASE("TemporaryDirectory moves cleanup ownership", "[test][filesystem]")
{
    std::filesystem::path moved_path;
    {
        jb::test::TemporaryDirectory source;
        moved_path = source.path();
        jb::test::TemporaryDirectory target{std::move(source)};
        CHECK(source.path().empty());
        CHECK(target.path() == moved_path);

        jb::test::TemporaryDirectory replacement;
        replacement = std::move(target);
        CHECK(target.path().empty());
        CHECK(replacement.path() == moved_path);
    }
    CHECK_FALSE(std::filesystem::exists(moved_path));
}

TEST_CASE("SequenceUuidGenerator returns configured values and stable exhaustion", "[test][uuid]")
{
    auto const                      first  = *Uuid::parse("00000000-0000-0000-0000-000000000001");
    auto const                      second = *Uuid::parse("00000000-0000-0000-0000-000000000002");
    jb::test::SequenceUuidGenerator generator{
        {first, second}
    };

    CHECK(*generator.generate() == first);
    CHECK(*generator.generate() == second);

    auto const exhausted = generator.generate();
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().category == ErrorCategory::ResourceExhausted);
    CHECK(exhausted.error().code == "test.uuid.sequence_exhausted");
}

TEST_CASE("FakeAttemptExecutor records owning starts and completes selected attempts explicitly",
          "[test][attempt-executor]")
{
    auto const cli_key  = attempt_key("00000000-0000-0000-0000-000000000001");
    auto const http_key = attempt_key("00000000-0000-0000-0000-000000000002", 2);

    jb::test::FakeAttemptExecutor executor;
    CHECK_FALSE(executor.is_available(jb::jobu::JobType::Cli));
    CHECK_FALSE(executor.is_available(jb::jobu::JobType::Http));
    executor.set_available(jb::jobu::JobType::Cli, true);
    CHECK(executor.is_available(jb::jobu::JobType::Cli));
    CHECK_FALSE(executor.is_available(jb::jobu::JobType::Http));
    executor.set_available(jb::jobu::JobType::Http, true);
    CHECK(executor.is_available(jb::jobu::JobType::Http));

    auto cli_completion  = std::optional<jb::jobu::AttemptCompletion>{};
    auto http_completion = std::optional<jb::jobu::AttemptCompletion>{};
    auto inside_start    = true;

    auto cli_started = executor.start(start_request(cli_key), [&](jb::jobu::AttemptCompletion completion) {
        CHECK_FALSE(inside_start);
        cli_completion = std::move(completion);
    });
    REQUIRE(cli_started);
    CHECK_FALSE(cli_completion);

    auto http_started =
        executor.start(start_request(http_key, jb::jobu::JobType::Http), [&](jb::jobu::AttemptCompletion completion) {
            CHECK_FALSE(inside_start);
            http_completion = std::move(completion);
        });
    REQUIRE(http_started);
    CHECK_FALSE(http_completion);
    inside_start = false;

    REQUIRE(executor.start_requests().size() == 2);
    CHECK(executor.start_requests()[0].key == cli_key);
    CHECK(executor.start_requests()[0].type == jb::jobu::JobType::Cli);
    CHECK(executor.start_requests()[0].payload == object_with_text("payload"));
    CHECK(executor.start_requests()[1].key == http_key);
    CHECK(executor.start_requests()[1].type == jb::jobu::JobType::Http);

    auto expected_pending = std::vector<jb::jobu::AttemptKey>{cli_key, http_key};
    CHECK(executor.pending_keys() == expected_pending);

    auto completed_http = executor.complete(http_key, attempt_completion(http_key));
    REQUIRE(completed_http);
    REQUIRE(http_completion);
    CHECK(http_completion->key == http_key);
    CHECK_FALSE(cli_completion);
    expected_pending = {cli_key};
    CHECK(executor.pending_keys() == expected_pending);

    auto completed_cli = executor.complete(cli_key, attempt_completion(cli_key));
    REQUIRE(completed_cli);
    REQUIRE(cli_completion);
    CHECK(cli_completion->key == cli_key);
    CHECK(executor.pending_keys().empty());
}

TEST_CASE("FakeAttemptExecutor retains no callback after failed or rejected starts", "[test][attempt-executor]")
{
    auto const key         = attempt_key("00000000-0000-0000-0000-000000000003");
    auto const start_error = Error{
        .category = ErrorCategory::Unavailable,
        .code     = "test.executor.start_injected",
        .message  = "Injected fake start failure",
    };

    jb::test::FakeAttemptExecutor executor;
    executor.set_start_error(start_error);
    auto callback_count = std::size_t{0};
    auto count_callback = [&](auto const&) { ++callback_count; };
    auto failed         = executor.start(start_request(key), count_callback);
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == start_error);
    CHECK(executor.start_requests().size() == 1);
    CHECK(executor.pending_keys().empty());
    CHECK(callback_count == 0);

    executor.set_start_error(std::nullopt);
    executor.set_available(jb::jobu::JobType::Cli, true);
    auto empty_handler = executor.start(start_request(key), {});
    REQUIRE_FALSE(empty_handler);
    CHECK(empty_handler.error().code == "test.executor.empty_completion_handler");
    CHECK(executor.start_requests().size() == 2);
    CHECK(executor.pending_keys().empty());

    auto invalid_key           = key;
    invalid_key.attempt_number = 0;
    auto invalid_start         = executor.start(start_request(invalid_key), count_callback);
    REQUIRE_FALSE(invalid_start);
    CHECK(invalid_start.error().code == "test.executor.invalid_key");
    CHECK(executor.start_requests().size() == 3);
    CHECK(executor.pending_keys().empty());

    REQUIRE(executor.start(start_request(key), count_callback));
    auto duplicate = executor.start(start_request(key), count_callback);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "test.executor.duplicate_start");
    CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{key});
    CHECK(callback_count == 0);
}

TEST_CASE("FakeAttemptExecutor rejects unavailable types after recording their starts", "[test][attempt-executor]")
{
    auto const cli_key  = attempt_key("00000000-0000-0000-0000-000000000009");
    auto const http_key = attempt_key("00000000-0000-0000-0000-00000000000a");

    jb::test::FakeAttemptExecutor executor;
    auto                          callback_count = std::size_t{0};
    auto                          count_callback = [&](auto const&) { ++callback_count; };

    auto unavailable_cli = executor.start(start_request(cli_key), count_callback);
    REQUIRE_FALSE(unavailable_cli);
    CHECK(unavailable_cli.error().category == ErrorCategory::Unavailable);
    CHECK(unavailable_cli.error().code == "test.executor.type_unavailable");

    auto unavailable_http = executor.start(start_request(http_key, jb::jobu::JobType::Http), count_callback);
    REQUIRE_FALSE(unavailable_http);
    CHECK(unavailable_http.error().category == ErrorCategory::Unavailable);
    CHECK(unavailable_http.error().code == "test.executor.type_unavailable");

    REQUIRE(executor.start_requests().size() == 2);
    CHECK(executor.start_requests()[0].key == cli_key);
    CHECK(executor.start_requests()[1].key == http_key);
    CHECK(executor.pending_keys().empty());
    CHECK(callback_count == 0);
}

TEST_CASE("FakeAttemptExecutor rejects mismatched unknown and duplicate completions", "[test][attempt-executor]")
{
    auto const pending_key = attempt_key("00000000-0000-0000-0000-000000000004");
    auto const other_key   = attempt_key("00000000-0000-0000-0000-000000000005");

    jb::test::FakeAttemptExecutor executor;
    auto                          callback_count = std::size_t{0};
    auto                          count_callback = [&](auto const&) { ++callback_count; };
    executor.set_available(jb::jobu::JobType::Cli, true);
    REQUIRE(executor.start(start_request(pending_key), count_callback));

    auto mismatched = executor.complete(pending_key, attempt_completion(other_key));
    REQUIRE_FALSE(mismatched);
    CHECK(mismatched.error().code == "test.executor.completion_key_mismatch");
    CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{pending_key});

    auto unknown = executor.complete(other_key, attempt_completion(other_key));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "test.executor.unknown_key");
    CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{pending_key});

    REQUIRE(executor.complete(pending_key, attempt_completion(pending_key)));
    CHECK(callback_count == 1);

    auto duplicate = executor.complete(pending_key, attempt_completion(pending_key));
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "test.executor.duplicate_completion");
    CHECK(callback_count == 1);
}

TEST_CASE("FakeAttemptExecutor accepts every valid completion shape", "[test][attempt-executor]")
{
    auto const key = attempt_key("00000000-0000-0000-0000-000000000006");

    auto completions = std::vector<jb::jobu::AttemptCompletion>{};
    completions.push_back(attempt_completion(key, jb::jobu::AttemptOutcome::Succeeded));

    auto terminal_failure                = attempt_completion(key, jb::jobu::AttemptOutcome::Failed);
    terminal_failure.failure_disposition = jb::jobu::FailureDisposition::Terminal;
    completions.push_back(std::move(terminal_failure));

    auto retryable_failure                = attempt_completion(key, jb::jobu::AttemptOutcome::Failed);
    retryable_failure.failure_disposition = jb::jobu::FailureDisposition::Retryable;
    retryable_failure.retry_not_before    = UtcTimePoint{200s};
    completions.push_back(std::move(retryable_failure));

    completions.push_back(attempt_completion(key, jb::jobu::AttemptOutcome::Cancelled));

    auto empty_serialized = jb::core::serialize_json(object_with_text(""));
    REQUIRE(empty_serialized);
    REQUIRE(empty_serialized->size() < kMaximumResultBytes);
    auto maximum_result   = attempt_completion(key);
    maximum_result.result = object_with_text(std::string(kMaximumResultBytes - empty_serialized->size(), 'x'));
    completions.push_back(std::move(maximum_result));

    for (auto& completion : completions) {
        jb::test::FakeAttemptExecutor executor;
        auto                          callback_count = std::size_t{0};
        auto                          count_callback = [&](auto const&) { ++callback_count; };
        executor.set_available(jb::jobu::JobType::Cli, true);
        REQUIRE(executor.start(start_request(key), count_callback));
        REQUIRE(executor.complete(key, std::move(completion)));
        CHECK(callback_count == 1);
        CHECK(executor.pending_keys().empty());
    }
}

TEST_CASE("FakeAttemptExecutor rejects inconsistent or unsafe completion shapes", "[test][attempt-executor]")
{
    auto const key = attempt_key("00000000-0000-0000-0000-000000000007");

    auto completions = std::vector<std::pair<std::string, jb::jobu::AttemptCompletion>>{};

    auto succeeded_fields                = attempt_completion(key, jb::jobu::AttemptOutcome::Succeeded);
    succeeded_fields.failure_disposition = jb::jobu::FailureDisposition::Terminal;
    completions.emplace_back("succeeded_fields", std::move(succeeded_fields));

    auto missing_disposition = attempt_completion(key, jb::jobu::AttemptOutcome::Failed);
    completions.emplace_back("missing_failure_disposition", std::move(missing_disposition));

    auto terminal_deadline                = attempt_completion(key, jb::jobu::AttemptOutcome::Failed);
    terminal_deadline.failure_disposition = jb::jobu::FailureDisposition::Terminal;
    terminal_deadline.retry_not_before    = UtcTimePoint{200s};
    completions.emplace_back("terminal_retry_deadline", std::move(terminal_deadline));

    auto cancelled_fields                = attempt_completion(key, jb::jobu::AttemptOutcome::Cancelled);
    cancelled_fields.failure_disposition = jb::jobu::FailureDisposition::Retryable;
    completions.emplace_back("cancelled_fields", std::move(cancelled_fields));

    auto interrupted = attempt_completion(key, jb::jobu::AttemptOutcome::Interrupted);
    completions.emplace_back("interrupted_reserved", std::move(interrupted));

    auto unknown_outcome    = attempt_completion(key);
    unknown_outcome.outcome = static_cast<jb::jobu::AttemptOutcome>(0xff);
    completions.emplace_back("unknown_outcome", std::move(unknown_outcome));

    auto unknown_disposition                = attempt_completion(key, jb::jobu::AttemptOutcome::Failed);
    unknown_disposition.failure_disposition = static_cast<jb::jobu::FailureDisposition>(0xff);
    completions.emplace_back("unknown_failure_disposition", std::move(unknown_disposition));

    auto scalar_result   = attempt_completion(key);
    scalar_result.result = make_json(std::string{"not an object"});
    completions.emplace_back("result_not_object", std::move(scalar_result));

    auto invalid_json   = attempt_completion(key);
    invalid_json.result = object_with_text(std::string{"\xc3\x28", 2});
    completions.emplace_back("result_not_serializable", std::move(invalid_json));

    auto oversized_result   = attempt_completion(key);
    oversized_result.result = object_with_text(std::string(kMaximumResultBytes, 'x'));
    completions.emplace_back("result_too_large", std::move(oversized_result));

    for (auto& [reason, completion] : completions) {
        jb::test::FakeAttemptExecutor executor;
        auto                          callback_count = std::size_t{0};
        auto                          count_callback = [&](auto const&) { ++callback_count; };
        executor.set_available(jb::jobu::JobType::Cli, true);
        REQUIRE(executor.start(start_request(key), count_callback));

        auto result = executor.complete(key, std::move(completion));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "test.executor.invalid_completion");
        CHECK(result.error().detail == reason);
        CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{key});
        CHECK(callback_count == 0);
    }
}

TEST_CASE("FakeAttemptExecutor cancellation records calls without completing attempts", "[test][attempt-executor]")
{
    auto const key          = attempt_key("00000000-0000-0000-0000-000000000008");
    auto const cancel_error = Error{
        .category = ErrorCategory::Unavailable,
        .code     = "test.executor.cancel_injected",
        .message  = "Injected fake cancellation failure",
    };

    jb::test::FakeAttemptExecutor executor;
    auto                          callback_count = std::size_t{0};
    auto                          count_callback = [&](auto const&) { ++callback_count; };
    executor.set_available(jb::jobu::JobType::Cli, true);
    REQUIRE(executor.start(start_request(key), count_callback));

    executor.set_cancel_error(cancel_error);
    auto failed = executor.cancel(key);
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == cancel_error);
    CHECK(executor.cancel_calls() == std::vector<jb::jobu::AttemptKey>{key});
    CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{key});
    CHECK(callback_count == 0);

    executor.set_cancel_error(std::nullopt);
    REQUIRE(executor.cancel(key));
    CHECK(executor.cancel_calls() == std::vector<jb::jobu::AttemptKey>{key, key});
    CHECK(executor.pending_keys() == std::vector<jb::jobu::AttemptKey>{key});
    CHECK(callback_count == 0);

    REQUIRE(executor.complete(key, attempt_completion(key, jb::jobu::AttemptOutcome::Cancelled)));
    CHECK(callback_count == 1);
}
