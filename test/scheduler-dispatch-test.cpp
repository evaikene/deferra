#include "scheduler_dispatch_priv.hpp"

#include "attribute_codec_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "support/fake_attempt_executor.hpp"
#include "support/fake_database_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::rpc;
using namespace jb::test;

namespace {

auto id(std::uint8_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[15]  = static_cast<std::byte>(suffix);
    return Uuid{bytes};
}

auto at(std::int64_t microseconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::microseconds{microseconds}};
}

auto materialized_attributes(StandardAttributeRegistry const& registry) -> std::string
{
    auto attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(attributes);
    auto document = encode_and_serialize_attribute_document(registry,
                                                            *attributes,
                                                            AttributeScope::Job,
                                                            AttributeDocumentMode::Materialized);
    REQUIRE(document);
    return std::string{document->serialized()};
}

auto dispatch_record(StandardAttributeRegistry const& registry) -> Record
{
    return Record{
        {
         Field{"run_id", uuid_to_storage(id(1))},
         Field{"run_job_id", uuid_to_storage(id(2))},
         Field{"run_job_revision", std::int64_t{1}},
         Field{"run_queue_id", uuid_to_storage(id(3))},
         Field{"run_origin", make_text("scheduled")},
         Field{"run_schedule_owned", std::int64_t{1}},
         Field{"run_planned_at_us", std::int64_t{100}},
         Field{"run_runnable_at_us", std::int64_t{100}},
         Field{"run_started_at_us", Null{}},
         Field{"run_completed_at_us", Null{}},
         Field{"run_type", make_text("cli")},
         Field{"run_priority", std::int64_t{7}},
         Field{"run_attributes_json", make_text(materialized_attributes(registry))},
         Field{"run_payload_json", make_text("{}")},
         Field{"run_state", make_text("scheduled")},
         Field{"run_result_json", Null{}},
         Field{"job_queue_id", uuid_to_storage(id(3))},
         Field{"job_state", make_text("active")},
         Field{"queue_state", make_text("active")},
         Field{"active_attempt_count", std::int64_t{0}},
         Field{"running_attempt_count", std::int64_t{0}},
         Field{"completed_attempt_count", std::int64_t{0}},
         Field{"failed_attempt_count", std::int64_t{0}},
         Field{"total_attempt_count", std::int64_t{0}},
         Field{"manual_sibling_count", std::int64_t{0}},
         Field{"schedule_sibling_count", std::int64_t{1}},
         }
    };
}

auto queue_record() -> Record
{
    return Record{
        {
         Field{"queue_id", uuid_to_storage(id(3))},
         Field{"queue_name", make_text("dispatch")},
         Field{"queue_state", make_text("active")},
         Field{"queue_weight", std::int64_t{1}},
         Field{"queue_concurrency_limit", std::int64_t{2}},
         Field{"queue_recovery_policy", make_text("fail_interrupted")},
         Field{"queue_defaults_json", make_text(R"({"version":1,"values":{}})")},
         Field{"queue_retention_seconds", Null{}},
         Field{"queue_runnable_wait_warning_ms", std::int64_t{10000}},
         Field{"queue_created_at_us", std::int64_t{0}},
         Field{"queue_updated_at_us", std::int64_t{0}},
         Field{"queue_deleted_at_us", Null{}},
         }
    };
}

auto select_plan(std::vector<std::string> parameters, std::vector<Record> records) -> FakeDatabaseQueryPlan
{
    return {
        .parameter_names = std::move(parameters),
        .execution_info  = {.produces_records = true, .rows_affected = -1},
        .records         = std::move(records),
    };
}

auto mutation_plan(std::vector<std::string> parameters, std::int64_t rows_affected = 1) -> FakeDatabaseQueryPlan
{
    return {
        .parameter_names = std::move(parameters),
        .execution_info  = {.produces_records = false, .rows_affected = rows_affected},
    };
}

void configure_dispatch_plans(FakeDatabaseDriverState& state, StandardAttributeRegistry const& registry)
{
    state.query_plans = {
        select_plan({":run_id"}, {dispatch_record(registry)}),
        select_plan({":id"}, {queue_record()}),
        select_plan({":queue_id", ":limit"}, {}),
        select_plan({":run_id"}, {Record{{Field{"maximum_attempt_number", Null{}}}}}),
        mutation_plan({":run_id",
                       ":attempt_number",
                       ":due_at_us",
                       ":started_at_us",
                       ":completed_at_us",
                       ":state",
                       ":outcome",
                       ":result_json"}),
        mutation_plan({":started_at_us", ":run_id", ":expected_state"}),
    };
}

auto test_error(std::string code) -> Error
{
    return {
        .category = ErrorCategory::Internal,
        .code     = std::move(code),
        .message  = "Injected dispatch failure",
        .detail   = "private-detail",
    };
}

auto call_position(FakeDatabaseDriverState const& state, std::string const& call) -> std::size_t
{
    auto const found = std::find(state.calls.begin(), state.calls.end(), call);
    REQUIRE(found != state.calls.end());
    return static_cast<std::size_t>(found - state.calls.begin());
}

class TracingExecutor final : public AttemptExecutor {
public:
    explicit TracingExecutor(std::shared_ptr<FakeDatabaseDriverState> state)
        : _state{std::move(state)}
    {}

    FakeAttemptExecutor fake;

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool override { return fake.is_available(type); }

    [[nodiscard]] auto start(AttemptStartRequest request, AttemptCompletionHandler completion)
        -> Result<void, Error> override
    {
        _state->calls.emplace_back("executor.start");
        return fake.start(std::move(request), std::move(completion));
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> Result<void, Error> override { return fake.cancel(key); }

private:
    std::shared_ptr<FakeDatabaseDriverState> _state;
};

struct DispatchFixture {
    std::shared_ptr<FakeDatabaseDriverState> state{std::make_shared<FakeDatabaseDriverState>()};
    Database                                 database{std::make_unique<FakeDatabaseDriver>(state)};
    StandardAttributeRegistry                registry;
    TracingExecutor                          executor{state};

    DispatchFixture()
    {
        REQUIRE(database.open());
        configure_dispatch_plans(*state, registry);
        executor.fake.set_available(JobType::Cli, true);
    }
};

auto dispatch(DispatchFixture& fixture) -> Result<std::optional<DispatchStart>, Error>
{
    return dispatch_selected(fixture.database, fixture.registry, fixture.executor, id(1), at(120), [](auto) {});
}

void require_no_start(DispatchFixture& fixture)
{
    CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "executor.start") ==
          fixture.state->calls.end());
    CHECK(fixture.executor.fake.start_requests().empty());
}

} // anonymous namespace

TEST_CASE("Atomic dispatch commits before invoking the executor", "[jobu][scheduler][dispatch]")
{
    DispatchFixture fixture;
    auto            result = dispatch(fixture);
    REQUIRE(result);
    REQUIRE(result->has_value());
    CHECK(result->value().key == AttemptKey{.run_id = id(1), .attempt_number = 1});
    CHECK_FALSE(result->value().immediate_completion);

    REQUIRE(fixture.executor.fake.start_requests().size() == 1U);
    auto const& request = fixture.executor.fake.start_requests().front();
    CHECK(request.key == result->value().key);
    CHECK(request.job_id == id(2));
    CHECK(request.queue_id == id(3));
    CHECK(request.type == JobType::Cli);
    CHECK(request.started_at == at(120));
    CHECK(request.payload.is_object());
    CHECK(call_position(*fixture.state, "driver.commit") < call_position(*fixture.state, "executor.start"));
    CHECK(fixture.state->last_transaction_mode == TransactionMode::Immediate);
}

TEST_CASE("Atomic dispatch returns a safe synthetic completion for executor start failure",
          "[jobu][scheduler][dispatch]")
{
    DispatchFixture fixture;
    fixture.executor.fake.set_start_error(test_error("test.executor.start_failed"));

    auto result = dispatch(fixture);
    REQUIRE(result);
    REQUIRE(result->has_value());
    REQUIRE(result->value().immediate_completion);
    auto const& completion = *result->value().immediate_completion;
    CHECK(completion.key == result->value().key);
    CHECK(completion.outcome == AttemptOutcome::Failed);
    CHECK(completion.failure_disposition == FailureDisposition::Terminal);
    CHECK_FALSE(completion.retry_not_before);
    REQUIRE(completion.result.is_object());
    auto const& object = completion.result.as_object();
    REQUIRE(object.size() == 2U);
    CHECK(object.at("error_code").as_string() == "test.executor.start_failed");
    CHECK(object.at("message").as_string() == "Injected dispatch failure");
    CHECK(call_position(*fixture.state, "driver.commit") < call_position(*fixture.state, "executor.start"));
    CHECK(fixture.executor.fake.pending_keys().empty());
}

TEST_CASE("Atomic dispatch rolls back normal revalidation loss without starting", "[jobu][scheduler][dispatch]")
{
    DispatchFixture fixture;
    fixture.state->query_plans.back().execution_info.rows_affected = 0;

    auto result = dispatch(fixture);
    REQUIRE(result);
    CHECK_FALSE(result->has_value());
    require_no_start(fixture);
    CHECK(call_position(*fixture.state, "driver.rollback") > call_position(*fixture.state, "query.exec"));
}

TEST_CASE("Atomic dispatch never starts after transaction or storage failure", "[jobu][scheduler][dispatch]")
{
    SECTION("begin")
    {
        DispatchFixture fixture;
        fixture.state->begin_error = test_error("db.fake.begin");
        auto result                = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.begin");
        require_no_start(fixture);
    }

    SECTION("context read")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[0].exec_error = test_error("db.fake.context");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.context");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("attempt insert")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[4].exec_error = test_error("db.fake.insert");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.insert");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("queue read")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[1].exec_error = test_error("db.fake.queue");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.storage.invariant");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("capacity read")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[2].exec_error = test_error("db.fake.capacity");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.capacity");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("attempt allocation")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[3].exec_error = test_error("db.fake.attempt_number");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.attempt_number");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("run update")
    {
        DispatchFixture fixture;
        fixture.state->query_plans[5].exec_error = test_error("db.fake.update");
        auto result                              = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.update");
        require_no_start(fixture);
        CHECK(std::find(fixture.state->calls.begin(), fixture.state->calls.end(), "driver.rollback") !=
              fixture.state->calls.end());
    }

    SECTION("commit")
    {
        DispatchFixture fixture;
        fixture.state->commit_error = test_error("db.fake.commit");
        auto result                 = dispatch(fixture);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "db.fake.commit");
        require_no_start(fixture);
        CHECK(call_position(*fixture.state, "driver.rollback") > call_position(*fixture.state, "driver.commit"));
    }
}
