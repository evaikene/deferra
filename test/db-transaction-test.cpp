#include "support/fake_database_driver.hpp"

#include "database.hpp"
#include "query.hpp"
#include "transaction.hpp"
#include "transaction_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace jb::core;
using namespace jb::db;
using namespace jb::test;

namespace {

struct OpenDatabase {
    std::shared_ptr<FakeDatabaseDriverState> state{std::make_shared<FakeDatabaseDriverState>()};
    Database                                 database{std::make_unique<FakeDatabaseDriver>(state)};

    OpenDatabase() { REQUIRE(database.open()); }
};

auto test_error(std::string code) -> Error
{
    return {
        .category = ErrorCategory::Internal,
        .code     = std::move(code),
        .message  = "Injected fake-driver failure",
    };
}

auto call_count(FakeDatabaseDriverState const& state, std::string const& call) -> std::size_t
{
    return static_cast<std::size_t>(std::count(state.calls.begin(), state.calls.end(), call));
}

auto call_position(FakeDatabaseDriverState const& state, std::string const& call) -> std::size_t
{
    auto const position = std::ranges::find(state.calls, call);
    REQUIRE(position != state.calls.end());
    return static_cast<std::size_t>(std::distance(state.calls.begin(), position));
}

} // anonymous namespace

TEST_CASE("Database directly begins commits and rolls back one transaction", "[db][transaction]")
{
    OpenDatabase fixture;

    REQUIRE(fixture.database.transaction(TransactionMode::Deferred));
    REQUIRE(fixture.state->last_transaction_mode);
    CHECK(*fixture.state->last_transaction_mode == TransactionMode::Deferred);

    auto nested = fixture.database.transaction();
    REQUIRE_FALSE(nested);
    CHECK(nested.error().code == "db.transaction_active");
    CHECK(call_count(*fixture.state, "driver.begin") == 1);

    REQUIRE(fixture.database.commit());
    CHECK(call_count(*fixture.state, "driver.commit") == 1);

    auto missing_commit = fixture.database.commit();
    REQUIRE_FALSE(missing_commit);
    CHECK(missing_commit.error().code == "db.no_transaction");

    REQUIRE(fixture.database.transaction(TransactionMode::Exclusive));
    CHECK(*fixture.state->last_transaction_mode == TransactionMode::Exclusive);
    REQUIRE(fixture.database.rollback());
    CHECK(call_count(*fixture.state, "driver.rollback") == 1);

    auto missing_rollback = fixture.database.rollback();
    REQUIRE_FALSE(missing_rollback);
    CHECK(missing_rollback.error().code == "db.no_transaction");
}

TEST_CASE("Direct transaction failures preserve commit recovery and poison failed rollback", "[db][transaction]")
{
    SECTION("failed begin and commit propagate driver errors")
    {
        OpenDatabase fixture;
        fixture.state->begin_error = test_error("db.fake.begin");

        auto begun = fixture.database.transaction();
        REQUIRE_FALSE(begun);
        CHECK(begun.error().code == "db.fake.begin");

        fixture.state->begin_error.reset();
        REQUIRE(fixture.database.transaction());
        fixture.state->commit_error = test_error("db.fake.commit");

        auto committed = fixture.database.commit();
        REQUIRE_FALSE(committed);
        CHECK(committed.error().code == "db.fake.commit");

        fixture.state->commit_error.reset();
        REQUIRE(fixture.database.rollback());
    }

    SECTION("failed rollback poisons operations until close")
    {
        OpenDatabase fixture;
        REQUIRE(fixture.database.transaction());
        fixture.state->rollback_error = test_error("db.fake.rollback");

        auto rolled_back = fixture.database.rollback();
        REQUIRE_FALSE(rolled_back);
        CHECK(rolled_back.error().code == "db.fake.rollback");

        auto const begin_calls = call_count(*fixture.state, "driver.begin");
        auto       poisoned    = fixture.database.transaction();
        REQUIRE_FALSE(poisoned);
        CHECK(poisoned.error().code == "db.connection_failed");
        CHECK(call_count(*fixture.state, "driver.begin") == begin_calls);

        {
            Query query{fixture.database};
            auto  executed = query.exec("SELECT 1");
            REQUIRE_FALSE(executed);
            CHECK(executed.error().code == "db.connection_failed");
            CHECK(call_count(*fixture.state, "driver.create_query") == 0);
        }

        fixture.state->rollback_error.reset();
        REQUIRE(fixture.database.close());
        REQUIRE(fixture.database.open());
        REQUIRE(fixture.database.transaction());
        REQUIRE(fixture.database.rollback());
    }
}

TEST_CASE("Database close and destruction roll back directly owned transactions", "[db][transaction]")
{
    SECTION("close rolls back before closing the driver")
    {
        OpenDatabase fixture;
        REQUIRE(fixture.database.transaction());
        REQUIRE(fixture.database.close());

        CHECK(call_position(*fixture.state, "driver.rollback") < call_position(*fixture.state, "driver.close"));
        CHECK_FALSE(fixture.database.is_open());
    }

    SECTION("destruction rolls back before closing the driver")
    {
        auto state = std::make_shared<FakeDatabaseDriverState>();
        {
            Database database{std::make_unique<FakeDatabaseDriver>(state)};
            REQUIRE(database.open());
            REQUIRE(database.transaction());
        }

        CHECK(call_position(*state, "driver.rollback") < call_position(*state, "driver.close"));
        CHECK_FALSE(state->open);
    }
}

TEST_CASE("Transaction guard rolls back on scope exit and transfers ownership by move", "[db][transaction][guard]")
{
    OpenDatabase fixture;
    {
        auto begun = Transaction::begin(fixture.database, TransactionMode::Deferred);
        REQUIRE(begun);
        auto source = std::move(begun).value();
        CHECK(source.is_active());

        Transaction moved{std::move(source)};
        CHECK_FALSE(source.is_active());
        CHECK(moved.is_active());
    }

    CHECK(call_count(*fixture.state, "driver.begin") == 1);
    CHECK(call_count(*fixture.state, "driver.rollback") == 1);
    CHECK(call_count(*fixture.state, "driver.commit") == 0);
}

TEST_CASE("Transaction guard supports explicit commit rollback and commit recovery", "[db][transaction][guard]")
{
    SECTION("commit deactivates the guard")
    {
        OpenDatabase fixture;
        auto         begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto guard = std::move(begun).value();

        REQUIRE(guard.commit());
        CHECK_FALSE(guard.is_active());
        CHECK(call_count(*fixture.state, "driver.commit") == 1);
        CHECK(call_count(*fixture.state, "driver.rollback") == 0);
    }

    SECTION("rollback deactivates the guard")
    {
        OpenDatabase fixture;
        auto         begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto guard = std::move(begun).value();

        REQUIRE(guard.rollback());
        CHECK_FALSE(guard.is_active());
        CHECK(call_count(*fixture.state, "driver.rollback") == 1);
    }

    SECTION("failed commit leaves the guard active for rollback")
    {
        OpenDatabase fixture;
        auto         begun = Transaction::begin(fixture.database);
        REQUIRE(begun);
        auto guard                  = std::move(begun).value();
        fixture.state->commit_error = test_error("db.fake.commit");

        auto committed = guard.commit();
        REQUIRE_FALSE(committed);
        CHECK(guard.is_active());

        fixture.state->commit_error.reset();
        REQUIRE(guard.rollback());
        CHECK_FALSE(guard.is_active());
    }
}

TEST_CASE("Transaction guard blocks direct completion close and database move", "[db][transaction][guard]")
{
    OpenDatabase fixture;
    auto         begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto guard = std::move(begun).value();

    auto nested = Transaction::begin(fixture.database);
    REQUIRE_FALSE(nested);
    CHECK(nested.error().code == "db.transaction_active");
    CHECK(call_count(*fixture.state, "driver.begin") == 1);

    auto committed = fixture.database.commit();
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == "db.transaction_guard_active");

    auto rolled_back = fixture.database.rollback();
    REQUIRE_FALSE(rolled_back);
    CHECK(rolled_back.error().code == "db.transaction_guard_active");

    auto closed = fixture.database.close();
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == "db.transaction_guard_active");

    Database moved{std::move(fixture.database)};
    CHECK_FALSE(moved.is_valid());
    CHECK(fixture.database.is_valid());
    REQUIRE(fixture.database.last_error());
    CHECK(fixture.database.last_error()->code == "db.transaction_guard_active");

    REQUIRE(guard.rollback());
}

TEST_CASE("Database move preserves a directly owned transaction", "[db][transaction]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database source{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(source.open());
    REQUIRE(source.transaction());

    Database moved{std::move(source)};
    CHECK_FALSE(source.is_valid());
    CHECK(moved.is_open());
    REQUIRE(moved.rollback());
    REQUIRE(moved.close());
}

TEST_CASE("Guard rollback failure deactivates the guard and poisons its database", "[db][transaction][guard]")
{
    OpenDatabase fixture;
    auto         begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto guard                    = std::move(begun).value();
    fixture.state->rollback_error = test_error("db.fake.rollback");

    auto rolled_back = guard.rollback();
    REQUIRE_FALSE(rolled_back);
    CHECK(rolled_back.error().code == "db.fake.rollback");
    CHECK_FALSE(guard.is_active());

    auto poisoned = Transaction::begin(fixture.database);
    REQUIRE_FALSE(poisoned);
    CHECK(poisoned.error().code == "db.connection_failed");

    fixture.state->rollback_error.reset();
    REQUIRE(fixture.database.close());
}

TEST_CASE("Guard token mismatch poisons the database", "[db][transaction][guard]")
{
    OpenDatabase fixture;
    auto         begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto guard = std::move(begun).value();
    detail::TransactionAccess::replace_token(guard, 999);

    auto committed = guard.commit();
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == "db.transaction_owner_mismatch");
    CHECK_FALSE(guard.is_active());

    auto poisoned = fixture.database.transaction();
    REQUIRE_FALSE(poisoned);
    CHECK(poisoned.error().code == "db.connection_failed");
    REQUIRE(fixture.database.close());
}

TEST_CASE("Transaction completion does not silently finish active queries", "[db][transaction][query]")
{
    OpenDatabase fixture;
    REQUIRE(fixture.database.transaction());
    Query query{fixture.database};
    REQUIRE(query.exec("SELECT 1"));
    CHECK(query.is_active());

    REQUIRE(fixture.database.commit());
    CHECK(query.is_active());
    CHECK(call_count(*fixture.state, "query.finish") == 0);

    REQUIRE(query.finish());
}

TEST_CASE("Transaction operations enforce database validity openness and thread affinity", "[db][transaction][thread]")
{
    Database invalid;
    auto     invalid_begin = invalid.transaction();
    REQUIRE_FALSE(invalid_begin);
    CHECK(invalid_begin.error().code == "db.invalid_database");

    auto     closed_state = std::make_shared<FakeDatabaseDriverState>();
    Database closed{std::make_unique<FakeDatabaseDriver>(closed_state)};
    auto     closed_begin = Transaction::begin(closed);
    REQUIRE_FALSE(closed_begin);
    CHECK(closed_begin.error().code == "db.database_closed");

    OpenDatabase fixture;
    auto const   calls_before = fixture.state->calls.size();
    std::string  error_code;
    std::thread  other_thread{[&] {
        auto begun = fixture.database.transaction();
        if (!begun) {
            error_code = begun.error().code;
        }
    }};
    other_thread.join();

    CHECK(error_code == "db.wrong_thread");
    CHECK(fixture.state->calls.size() == calls_before);
}

TEST_CASE("Inactive Transaction operations report no transaction", "[db][transaction][guard]")
{
    Transaction transaction;
    CHECK_FALSE(transaction.is_active());

    auto committed = transaction.commit();
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == "db.no_transaction");

    auto rolled_back = transaction.rollback();
    REQUIRE_FALSE(rolled_back);
    CHECK(rolled_back.error().code == "db.no_transaction");
}
