#include "attempt_executor_group.hpp"

#include "json.hpp"
#include "object.hpp"
#include "object_priv.hpp"
#include "support/fake_attempt_executor.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto test_error(ErrorCategory category, std::string code, std::string message) -> Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
    };
}

auto object_with_text(std::string text) -> JsonValue
{
    auto object = JsonValue::Object{};
    object.emplace("value", JsonValue{.data = std::move(text)});
    return JsonValue{.data = std::move(object)};
}

auto uuid(std::string_view value) -> Uuid
{
    auto parsed = Uuid::parse(value);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto attempt_key(std::string_view value, AttemptNumber attempt_number = 1) -> AttemptKey
{
    return {
        .run_id         = uuid(value),
        .attempt_number = attempt_number,
    };
}

auto first_key() -> AttemptKey
{
    return attempt_key("11111111-1111-4111-8111-111111111111");
}

auto second_key() -> AttemptKey
{
    return attempt_key("22222222-2222-4222-8222-222222222222");
}

auto third_key() -> AttemptKey
{
    return attempt_key("33333333-3333-4333-8333-333333333333");
}

auto start_request(AttemptKey key, JobType type) -> AttemptStartRequest
{
    return {
        .key        = key,
        .job_id     = uuid("44444444-4444-4444-8444-444444444444"),
        .queue_id   = uuid("55555555-5555-4555-8555-555555555555"),
        .type       = type,
        .attributes = {},
        .payload    = object_with_text("payload"),
        .started_at = UtcTimePoint{10s},
    };
}

auto completion(AttemptKey key) -> AttemptCompletion
{
    return {
        .key                 = key,
        .outcome             = AttemptOutcome::Succeeded,
        .failure_disposition = std::nullopt,
        .retry_not_before    = std::nullopt,
        .result              = object_with_text("result"),
    };
}

struct ObjectExecutorObservation {
    std::size_t             destructions{0};
    bool                    destroyed_with_pending_completion{false};
    bool                    available{true};
    std::vector<AttemptKey> cancellations;
    std::function<void()>   on_destroy;
};

class ObjectAttemptExecutor final : public Object, public AttemptExecutor {
public:
    ObjectAttemptExecutor(JobType type, ObjectExecutorObservation& observation, Object* parent = nullptr);
    ~ObjectAttemptExecutor() override;

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override;
    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion_handler)
        -> Result<void, Error> override;
    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override;

    [[nodiscard]] auto complete(AttemptCompletion value) -> Result<void, Error>;

private:
    struct Private;

    [[nodiscard]] auto data() const noexcept -> Private*;
};

struct ObjectAttemptExecutor::Private : jb::core::priv::ObjectPrivate {
    Private(JobType type_value, ObjectExecutorObservation& observation_value)
        : type{type_value}
        , observation{observation_value}
    {}

    JobType                    type;
    ObjectExecutorObservation& observation;
    std::optional<AttemptKey>  key;
    AttemptCompletionHandler   completion;
};

ObjectAttemptExecutor::ObjectAttemptExecutor(JobType type, ObjectExecutorObservation& observation, Object* parent)
    : Object{
          *new Private{type, observation},
          parent
}
{}

ObjectAttemptExecutor::~ObjectAttemptExecutor()
{
    auto* state                                          = data();
    state->observation.destroyed_with_pending_completion = static_cast<bool>(state->completion);
    if (state->observation.on_destroy) {
        state->observation.on_destroy();
    }
    state->completion = {};
    ++state->observation.destructions;
}

auto ObjectAttemptExecutor::is_available(JobType type) const noexcept -> bool
{
    auto const* state = data();
    return type == state->type && state->observation.available;
}

auto ObjectAttemptExecutor::start(AttemptStartRequest request, AttemptCompletionHandler completion_handler)
    -> Result<void, Error>
{
    using ExecutorResult = Result<void, Error>;

    auto* state = data();
    if (request.type != state->type || !completion_handler) {
        return ExecutorResult::failure(
            test_error(ErrorCategory::InvalidArgument, "test.object_executor.invalid_start", "Invalid test start"));
    }
    if (state->completion) {
        return ExecutorResult::failure(
            test_error(ErrorCategory::Conflict, "test.object_executor.duplicate_start", "Duplicate test start"));
    }

    state->key        = request.key;
    state->completion = std::move(completion_handler);
    return ExecutorResult::success();
}

auto ObjectAttemptExecutor::cancel(AttemptKey const& key) -> Result<void, Error>
{
    data()->observation.cancellations.push_back(key);
    return Result<void, Error>::success();
}

auto ObjectAttemptExecutor::complete(AttemptCompletion value) -> Result<void, Error>
{
    using ExecutorResult = Result<void, Error>;

    auto* state = data();
    if (!state->completion) {
        return ExecutorResult::failure(
            test_error(ErrorCategory::NotFound, "test.object_executor.not_pending", "No test completion is pending"));
    }

    auto handler = std::move(state->completion);
    state->key.reset();
    handler(std::move(value));
    return ExecutorResult::success();
}

auto ObjectAttemptExecutor::data() const noexcept -> Private*
{
    return d_ptr<Private>();
}

struct FakeExecutors {
    FakeAttemptExecutor* cli{nullptr};
    FakeAttemptExecutor* http{nullptr};
};

auto add_fake_executors(AttemptExecutorGroup& group) -> FakeExecutors
{
    auto  cli_owner = std::make_unique<FakeAttemptExecutor>();
    auto* cli       = cli_owner.get();
    cli->set_available(JobType::Cli, true);
    REQUIRE(group.add(JobType::Cli, std::move(cli_owner)));

    auto  http_owner = std::make_unique<FakeAttemptExecutor>();
    auto* http       = http_owner.get();
    http->set_available(JobType::Http, true);
    REQUIRE(group.add(JobType::Http, std::move(http_owner)));

    return {.cli = cli, .http = http};
}

} // anonymous namespace

TEST_CASE("Attempt executor group validates owned registration", "[jobu][executor-group]")
{
    AttemptExecutorGroup group;

    SECTION("null executor")
    {
        auto result = group.add(JobType::Cli, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::InvalidArgument);
        CHECK(result.error().code == "jobu.executor.invalid_registration");
    }

    SECTION("unknown type")
    {
        auto result = group.add(static_cast<JobType>(255), std::make_unique<FakeAttemptExecutor>());
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::Unsupported);
        CHECK(result.error().code == "jobu.executor.unsupported_type");
    }

    SECTION("duplicate type")
    {
        auto first = std::make_unique<FakeAttemptExecutor>();
        first->set_available(JobType::Cli, true);
        REQUIRE(group.add(JobType::Cli, std::move(first)));

        auto duplicate = group.add(JobType::Cli, std::make_unique<FakeAttemptExecutor>());
        REQUIRE_FALSE(duplicate);
        CHECK(duplicate.error().category == ErrorCategory::Conflict);
        CHECK(duplicate.error().code == "jobu.executor.duplicate_type");
        CHECK(group.is_available(JobType::Cli));
    }
}

TEST_CASE("Attempt executor group keeps Object ownership exclusive", "[jobu][executor-group][object]")
{
    SECTION("parented registration is destroyed and unlinked exactly once")
    {
        ObjectExecutorObservation observation;
        Object                    parent;

        {
            AttemptExecutorGroup group;
            auto                 child = std::make_unique<ObjectAttemptExecutor>(JobType::Cli, observation, &parent);
            CHECK(parent.children() == std::vector<Object*>{child.get()});

            auto result = group.add(JobType::Cli, std::move(child));
            REQUIRE_FALSE(result);
            CHECK(result.error().code == "jobu.executor.invalid_registration");
            CHECK(observation.destructions == 1U);
            CHECK(parent.children().empty());
            CHECK_FALSE(group.is_available(JobType::Cli));
        }

        CHECK(observation.destructions == 1U);
    }

    SECTION("accepted Object remains unparented until group destruction")
    {
        ObjectExecutorObservation observation;
        auto                      callback_count = std::size_t{0};

        {
            AttemptExecutorGroup group;
            auto                 child = std::make_unique<ObjectAttemptExecutor>(JobType::Cli, observation);
            auto*                raw   = child.get();
            REQUIRE(group.add(JobType::Cli, std::move(child)));
            CHECK(raw->parent() == nullptr);
            CHECK(group.is_available(JobType::Cli));

            auto completion_handler = [&](AttemptCompletion const&) { ++callback_count; };
            REQUIRE(group.start(start_request(first_key(), JobType::Cli), completion_handler));
        }

        CHECK(observation.destructions == 1U);
        CHECK(observation.destroyed_with_pending_completion);
        CHECK(callback_count == 0U);
    }
}

TEST_CASE("Attempt executor group routes availability starts and cancellation independently", "[jobu][executor-group]")
{
    AttemptExecutorGroup group;
    auto const           executors = add_fake_executors(group);

    CHECK(group.is_available(JobType::Cli));
    CHECK(group.is_available(JobType::Http));
    CHECK_FALSE(group.is_available(static_cast<JobType>(255)));

    executors.cli->set_available(JobType::Cli, false);
    CHECK_FALSE(group.is_available(JobType::Cli));
    CHECK(group.is_available(JobType::Http));
    executors.cli->set_available(JobType::Cli, true);

    auto       completions = std::vector<AttemptKey>{};
    auto const cli_key     = first_key();
    auto const http_key    = second_key();
    REQUIRE(group.start(start_request(cli_key, JobType::Cli),
                        [&](AttemptCompletion const& value) { completions.push_back(value.key); }));
    REQUIRE(group.start(start_request(http_key, JobType::Http),
                        [&](AttemptCompletion const& value) { completions.push_back(value.key); }));

    REQUIRE(executors.cli->start_requests().size() == 1U);
    CHECK(executors.cli->start_requests().front().type == JobType::Cli);
    REQUIRE(executors.http->start_requests().size() == 1U);
    CHECK(executors.http->start_requests().front().type == JobType::Http);

    // Availability may change after acceptance; cancellation must still use the recorded route.
    executors.cli->set_available(JobType::Cli, false);
    REQUIRE(group.cancel(cli_key));
    CHECK(executors.cli->cancel_calls() == std::vector<AttemptKey>{cli_key});
    CHECK(executors.http->cancel_calls().empty());

    REQUIRE(group.cancel(http_key));
    CHECK(executors.http->cancel_calls() == std::vector<AttemptKey>{http_key});

    REQUIRE(executors.cli->complete(cli_key, completion(cli_key)));
    REQUIRE(executors.http->complete(http_key, completion(http_key)));
    CHECK(completions == std::vector<AttemptKey>{cli_key, http_key});
}

TEST_CASE("Attempt executor group creates routes only for accepted unique starts", "[jobu][executor-group]")
{
    AttemptExecutorGroup group;
    auto const           executors = add_fake_executors(group);
    auto const           key       = first_key();

    REQUIRE(group.start(start_request(key, JobType::Cli), [](AttemptCompletion const&) {}));

    auto duplicate = group.start(start_request(key, JobType::Http), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "jobu.executor.duplicate_attempt");
    CHECK(executors.http->start_requests().empty());

    auto const empty_handler_key = second_key();
    auto       empty_handler     = group.start(start_request(empty_handler_key, JobType::Http), {});
    REQUIRE_FALSE(empty_handler);
    CHECK(empty_handler.error().code == "test.executor.empty_completion_handler");
    CHECK_FALSE(group.cancel(empty_handler_key));

    executors.http->set_start_error(Error{
        .category = ErrorCategory::Unavailable,
        .code     = "test.executor.rejected",
        .message  = "Safe test rejection",
    });
    auto const rejected_key = third_key();
    auto       rejected     = group.start(start_request(rejected_key, JobType::Http), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "test.executor.rejected");

    auto missing = group.cancel(rejected_key);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "jobu.executor.attempt_not_found");
}

TEST_CASE("Attempt executor group retires the accepted route before exact callback delivery", "[jobu][executor-group]")
{
    SECTION("callback can re-enter with the completed key on another executor")
    {
        AttemptExecutorGroup group;
        auto const           executors                = add_fake_executors(group);
        auto const           key                      = first_key();
        auto                 reentrant_start_accepted = false;
        auto                 callback_count           = std::size_t{0};

        auto reentrant_completion = [&](AttemptCompletion const&) { ++callback_count; };
        auto initial_completion   = [&](AttemptCompletion const&) {
            ++callback_count;
            auto restarted           = group.start(start_request(key, JobType::Http), reentrant_completion);
            reentrant_start_accepted = static_cast<bool>(restarted);
        };
        REQUIRE(group.start(start_request(key, JobType::Cli), initial_completion));

        REQUIRE(executors.cli->complete(key, completion(key)));
        CHECK(reentrant_start_accepted);
        CHECK(callback_count == 1U);
        CHECK(executors.http->pending_keys() == std::vector<AttemptKey>{key});

        REQUIRE(executors.http->complete(key, completion(key)));
        CHECK(callback_count == 2U);
    }

    SECTION("incorrect child identity is forwarded while the accepted route is retired")
    {
        ObjectExecutorObservation observation;
        AttemptExecutorGroup      group;
        auto                      child = std::make_unique<ObjectAttemptExecutor>(JobType::Cli, observation);
        auto*                     raw   = child.get();
        REQUIRE(group.add(JobType::Cli, std::move(child)));

        auto const accepted_key = first_key();
        auto const reported_key = second_key();
        auto       observed_key = std::optional<AttemptKey>{};
        REQUIRE(group.start(start_request(accepted_key, JobType::Cli),
                            [&](AttemptCompletion const& value) { observed_key = value.key; }));

        REQUIRE(raw->complete(completion(reported_key)));
        REQUIRE(observed_key);
        CHECK(*observed_key == reported_key);

        auto cancelled = group.cancel(accepted_key);
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == "jobu.executor.attempt_not_found");
    }
}

TEST_CASE("Attempt executor group shutdown is terminal even when empty", "[jobu][executor-group]")
{
    AttemptExecutorGroup group;
    group.shutdown();
    group.shutdown();

    auto check_stopping = [](Result<void, Error> const& result) {
        REQUIRE_FALSE(result);
        CHECK(result.error().category == ErrorCategory::Unavailable);
        CHECK(result.error().code == "jobu.executor.stopping");
        CHECK(result.error().detail.empty());
    };

    // Stopping takes precedence even when ordinary validation would reject the request for another reason.
    for (auto type : {JobType::Cli, JobType::Http, static_cast<JobType>(255)}) {
        CHECK_FALSE(group.is_available(type));
        check_stopping(group.add(type, {}));
        check_stopping(group.start(start_request(first_key(), type), {}));
    }
    check_stopping(group.cancel(first_key()));

    ObjectExecutorObservation rejected;
    check_stopping(group.add(JobType::Cli, std::make_unique<ObjectAttemptExecutor>(JobType::Cli, rejected)));
    CHECK(rejected.destructions == 1U);
}

TEST_CASE("Attempt executor group shutdown releases mixed runners while dependencies remain alive",
          "[jobu][executor-group]")
{
    ObjectExecutorObservation cli;
    ObjectExecutorObservation http;
    auto                      callback_count = std::size_t{0};
    auto                      cleanup_count  = std::size_t{0};

    // This borrowed dependency models the client/event-loop lifetime required by runner cleanup.
    auto dependency          = std::make_shared<int>(42);
    auto dependency_observer = std::weak_ptr<int>{dependency};
    auto retained_capture    = std::weak_ptr<int>{};
    {
        AttemptExecutorGroup group;
        auto                 during_cleanup = [&] {
            ++cleanup_count;
            CHECK_FALSE(dependency_observer.expired());
            CHECK_FALSE(group.is_available(JobType::Cli));
            CHECK_FALSE(group.is_available(JobType::Http));

            // Routes and children are being torn down: every operation must stop before consulting either map.
            auto cancelled = group.cancel(first_key());
            REQUIRE_FALSE(cancelled);
            CHECK(cancelled.error().code == "jobu.executor.stopping");
            auto started = group.start(start_request(third_key(), JobType::Http), {});
            REQUIRE_FALSE(started);
            CHECK(started.error().code == "jobu.executor.stopping");
            auto added = group.add(JobType::Cli, {});
            REQUIRE_FALSE(added);
            CHECK(added.error().code == "jobu.executor.stopping");
            group.shutdown();
        };
        cli.on_destroy  = during_cleanup;
        http.on_destroy = during_cleanup;
        REQUIRE(group.add(JobType::Cli, std::make_unique<ObjectAttemptExecutor>(JobType::Cli, cli)));
        REQUIRE(group.add(JobType::Http, std::make_unique<ObjectAttemptExecutor>(JobType::Http, http)));

        // Only the retained completion wrappers keep this capture alive after setup.
        {
            auto capture     = std::make_shared<int>(7);
            retained_capture = capture;
            auto handler     = [&, capture](AttemptCompletion const&) {
                callback_count += static_cast<std::size_t>(*capture);
            };
            REQUIRE(group.start(start_request(first_key(), JobType::Cli), handler));
            REQUIRE(group.start(start_request(second_key(), JobType::Http), handler));
        }
        CHECK_FALSE(retained_capture.expired());

        group.shutdown();
        CHECK(cli.destructions == 1U);
        CHECK(http.destructions == 1U);
        CHECK(cli.destroyed_with_pending_completion);
        CHECK(http.destroyed_with_pending_completion);
        CHECK(cli.cancellations.empty());
        CHECK(http.cancellations.empty());
        CHECK(retained_capture.expired());
        CHECK(callback_count == 0U);
        CHECK(cleanup_count == 2U);

        group.shutdown();
        CHECK(cleanup_count == 2U);
        // Explicit shutdown has released all runners; the empty group's later destructor needs no dependency.
        dependency.reset();
    }
    CHECK(dependency_observer.expired());
    CHECK(cli.destructions == 1U);
    CHECK(http.destructions == 1U);
    CHECK(callback_count == 0U);
    CHECK(cleanup_count == 2U);
}
