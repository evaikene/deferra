#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fault_database_driver.hpp"

#include "database.hpp"
#include "query.hpp"
#include "support/fake_database_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace jb::core;
using namespace jb::db;
using namespace jb::test;

namespace {

auto injected_error() -> Error
{
    return {.category = ErrorCategory::Io, .code = "db.io", .message = "Injected failure"};
}

struct Fixture {
    std::shared_ptr<FakeDatabaseDriverState> backend = std::make_shared<FakeDatabaseDriverState>();
    std::shared_ptr<DatabaseFaultState>      faults  = std::make_shared<DatabaseFaultState>();
    Database database{std::make_unique<FaultDatabaseDriver>(std::make_unique<FakeDatabaseDriver>(backend), faults)};
};

} // namespace

TEST_CASE("Database decorator forwards metadata values cursor state and transaction mode", "[db][fault]")
{
    Fixture fixture;
    fixture.backend->parameter_names = {":value"};
    fixture.backend->execution_info  = {.produces_records = true,
                                        .rows_affected    = 7,
                                        .record_metadata  = Record{{Field{"value", Null{}}}}};
    fixture.backend->records         = {Record{{Field{"value", std::int64_t{42}}}}};

    CHECK(fixture.database.driver_name() == "fake");
    REQUIRE(fixture.database.open());
    CHECK(fixture.database.is_open());
    REQUIRE(fixture.database.transaction(TransactionMode::Exclusive));
    CHECK(fixture.backend->last_transaction_mode == TransactionMode::Exclusive);
    {
        Query query{fixture.database};
        REQUIRE(query.prepare("SELECT :value"));
        REQUIRE(query.bind_value(":value", std::int64_t{42}));
        REQUIRE(query.exec());
        CHECK(query.is_select());
        CHECK(query.num_rows_affected() == 7);
        CHECK(query.record().field_name(0) == "value");
        CHECK(query.record().is_null(0));
        REQUIRE(query.next().value());
        CHECK(query.value(0) == Value{std::int64_t{42}});
        REQUIRE_FALSE(query.next().value());
        REQUIRE(query.finish());
        REQUIRE(query.exec());
        REQUIRE(query.next().value());
        CHECK(query.value(0) == Value{std::int64_t{42}});
        query.clear();
        CHECK_FALSE(query.is_active());
    }
    REQUIRE(fixture.database.commit());
    REQUIRE(fixture.database.transaction());
    REQUIRE(fixture.database.rollback());
    REQUIRE(fixture.database.close());
    CHECK_FALSE(fixture.database.is_open());
    CHECK(fixture.backend->prepared_sql == "SELECT :value");
    REQUIRE_FALSE(fixture.backend->bindings.empty());
    CHECK(fixture.backend->bindings.front() == std::pair<std::size_t, Value>{0, std::int64_t{42}});
    CHECK(fixture.backend->clear_count > 0);
    CHECK(std::ranges::count(fixture.backend->calls, "query.finish") >= 1);
}

TEST_CASE("Database faults select named boundaries and fire only once", "[db][fault]")
{
    Fixture fixture;
    fixture.faults->classify = [](std::string_view sql) { return sql == "target" ? "target" : "other"; };
    fixture.faults->faults.push_back({
        .at    = {.boundary = "target", .operation = DatabaseOperation::Execute},
        .error = injected_error()
    });
    REQUIRE(fixture.database.open());
    Query query{fixture.database};
    REQUIRE(query.exec("unrelated"));
    auto failed = query.exec("target");
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == injected_error());
    CHECK(fixture.faults->faults.front().fired);
    CHECK(std::ranges::count(fixture.backend->calls, "query.exec") == 1);
    REQUIRE(query.exec());
    CHECK(std::ranges::count(fixture.backend->calls, "query.exec") == 2);
}

TEST_CASE("Database commit faults distinguish precommit failure from acknowledgement loss", "[db][fault]")
{
    for (auto phase : {DatabaseFaultPhase::Before, DatabaseFaultPhase::AfterSuccess}) {
        Fixture fixture;
        fixture.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Commit, .phase = phase},
            .error = injected_error()
        });
        REQUIRE(fixture.database.open());
        REQUIRE(fixture.database.transaction());
        auto committed = fixture.database.commit();
        REQUIRE_FALSE(committed);
        CHECK(committed.error() == injected_error());
        CHECK(fixture.faults->faults.front().fired);
        CHECK(std::ranges::count(fixture.backend->calls, "driver.commit") ==
              (phase == DatabaseFaultPhase::Before ? 0 : 1));
        REQUIRE(fixture.database.rollback());
    }
}

TEST_CASE("Database decorator preserves backend errors before acknowledgement injection", "[db][fault]")
{
    Fixture fixture;
    auto    backend_error         = Error{.category = ErrorCategory::Conflict, .code = "db.busy", .message = "Busy"};
    fixture.backend->commit_error = backend_error;
    fixture.faults->faults.push_back({
        .at    = {.boundary  = "connection",
                  .operation = DatabaseOperation::Commit,
                  .phase     = DatabaseFaultPhase::AfterSuccess},
        .error = injected_error()
    });
    REQUIRE(fixture.database.open());
    REQUIRE(fixture.database.transaction());
    auto result = fixture.database.commit();
    REQUIRE_FALSE(result);
    CHECK(result.error() == backend_error);
    CHECK_FALSE(fixture.faults->faults.front().fired);
    REQUIRE(fixture.database.rollback());
}
