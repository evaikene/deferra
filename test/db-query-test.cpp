#include "support/fake_database_driver.hpp"

#include "database.hpp"
#include "query.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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

} // anonymous namespace

TEST_CASE("Query may be constructed before its database opens", "[db][query]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database database{std::make_unique<FakeDatabaseDriver>(state)};
    Query    query{database};

    auto prepared = query.prepare("SELECT 1");
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error().code == "db.database_closed");
    CHECK(call_count(*state, "driver.create_query") == 0);

    REQUIRE(database.open());
    REQUIRE(query.prepare("SELECT 1"));
    CHECK(query.last_query() == "SELECT 1");
}

TEST_CASE("Query prepare and direct execution propagate driver state", "[db][query]")
{
    OpenDatabase fixture;
    Query        query{fixture.database};

    fixture.state->execution_info = {
        .produces_records = false,
        .rows_affected    = 3,
    };

    REQUIRE(query.exec("UPDATE jobs SET priority = 1"));
    CHECK(query.is_active());
    CHECK_FALSE(query.is_valid());
    CHECK_FALSE(query.is_select());
    CHECK(query.num_rows_affected() == 3);
    CHECK(query.last_query() == "UPDATE jobs SET priority = 1");
    CHECK_THROWS_AS(query.record(), std::logic_error);
    CHECK_FALSE(query.last_error());

    REQUIRE(query.finish());
    CHECK_FALSE(query.is_active());
    CHECK(query.num_rows_affected() == -1);
    CHECK(query.last_query() == "UPDATE jobs SET priority = 1");
}

TEST_CASE("Query requires every positional parameter to be explicitly bound", "[db][query][binding]")
{
    OpenDatabase fixture;
    fixture.state->parameter_names = {"", ""};
    Query query{fixture.database};

    REQUIRE(query.prepare("INSERT INTO jobs VALUES (?, ?)"));
    REQUIRE(query.bind_value(0, std::int64_t{7}));

    auto incomplete = query.exec();
    REQUIRE_FALSE(incomplete);
    CHECK(incomplete.error().code == "db.parameter_unbound");
    CHECK(call_count(*fixture.state, "query.exec") == 0);

    REQUIRE(query.bind_value(1, Null{}));
    REQUIRE(query.exec());
    CHECK(call_count(*fixture.state, "query.bind") == 2);
    CHECK(call_count(*fixture.state, "query.exec") == 1);

    REQUIRE(query.exec());
    CHECK(call_count(*fixture.state, "query.bind") == 2);
    CHECK(call_count(*fixture.state, "query.exec") == 2);
}

TEST_CASE("Query supports sequential and named bindings without mixing modes", "[db][query][binding]")
{
    SECTION("sequential binding uses each next unbound slot")
    {
        OpenDatabase fixture;
        fixture.state->parameter_names = {"", ""};
        Query query{fixture.database};

        REQUIRE(query.prepare("INSERT INTO jobs VALUES (?, ?)"));
        REQUIRE(query.add_bind_value(std::int64_t{1}));
        REQUIRE(query.add_bind_value(make_text("second")));
        REQUIRE(query.exec());
        REQUIRE(fixture.state->bindings.size() == 2);
        CHECK(fixture.state->bindings[0].first == 0);
        CHECK(fixture.state->bindings[1].first == 1);
    }

    SECTION("named binding uses complete backend placeholders")
    {
        OpenDatabase fixture;
        fixture.state->parameter_names = {":id", ":name"};
        Query query{fixture.database};

        REQUIRE(query.prepare("INSERT INTO jobs VALUES (:id, :name)"));
        REQUIRE(query.bind_value(":name", make_text("named")));
        REQUIRE(query.bind_value(":id", std::int64_t{9}));
        REQUIRE(query.exec());

        auto mixed = query.bind_value(0, std::int64_t{10});
        REQUIRE_FALSE(mixed);
        CHECK(mixed.error().code == "db.mixed_binding");

        auto mixed_sequential = query.add_bind_value(std::int64_t{11});
        REQUIRE_FALSE(mixed_sequential);
        CHECK(mixed_sequential.error().code == "db.mixed_binding");
    }
}

TEST_CASE("Query reports invalid and failed binding without marking a slot bound", "[db][query][binding]")
{
    OpenDatabase fixture;
    fixture.state->parameter_names = {":value"};
    Query query{fixture.database};
    REQUIRE(query.prepare("SELECT :value"));

    auto empty = query.bind_value("", std::int64_t{0});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "db.invalid_parameter");

    auto unknown = query.bind_value(":missing", std::int64_t{1});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "db.invalid_parameter");

    fixture.state->bind_error = test_error("db.fake.bind");
    auto failed               = query.bind_value(":value", std::int64_t{2});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "db.fake.bind");
    REQUIRE(query.last_error());
    CHECK(query.last_error()->code == "db.fake.bind");

    fixture.state->bind_error.reset();
    auto unbound = query.exec();
    REQUIRE_FALSE(unbound);
    CHECK(unbound.error().code == "db.parameter_unbound");

    REQUIRE(query.bind_value(":value", std::int64_t{3}));
    CHECK_FALSE(query.last_error());
}

TEST_CASE("Query exposes rows and field metadata across cursor positions", "[db][query][cursor]")
{
    OpenDatabase fixture;
    fixture.state->execution_info = {
        .produces_records = true,
        .rows_affected    = -1,
        .record_metadata  = Record{{Field{"id", Null{}}, Field{"name", Null{}}}},
    };
    fixture.state->records = {
        Record{{Field{"id", std::int64_t{1}}, Field{"name", make_text("first")}}},
        Record{{Field{"id", std::int64_t{2}}, Field{"name", Null{}}}},
    };

    Query query{fixture.database};
    REQUIRE(query.exec("SELECT id, name FROM jobs"));
    CHECK(query.is_active());
    CHECK(query.is_select());
    CHECK_FALSE(query.is_valid());
    CHECK(query.record().count() == 2);
    CHECK(query.record().is_null("id"));
    CHECK(query.is_null(0));
    CHECK(query.value("id") == nullptr);
    CHECK_THROWS_AS(query.value(0), std::logic_error);

    REQUIRE(query.next().value());
    CHECK(query.is_valid());
    CHECK(std::get<std::int64_t>(query.value(0)) == 1);
    REQUIRE(query.value("name") != nullptr);
    CHECK(std::get<std::string>(*query.value("name")) == "first");
    CHECK_FALSE(query.is_null("name"));

    REQUIRE(query.next().value());
    CHECK(query.is_null("name"));

    CHECK_FALSE(query.next().value());
    CHECK(query.is_active());
    CHECK_FALSE(query.is_valid());
    CHECK(query.record().count() == 2);
    CHECK(query.record().is_null("id"));

    auto const fetch_calls = call_count(*fixture.state, "query.next");
    CHECK_FALSE(query.next().value());
    CHECK(call_count(*fixture.state, "query.next") == fetch_calls);
}

TEST_CASE("Query retains cursor state after fetch failure and can be finished", "[db][query][cursor]")
{
    OpenDatabase fixture;
    fixture.state->execution_info.produces_records = true;
    fixture.state->next_error                      = test_error("db.fake.fetch");
    Query query{fixture.database};

    REQUIRE(query.exec("SELECT value FROM jobs"));
    auto fetched = query.next();
    REQUIRE_FALSE(fetched);
    CHECK(fetched.error().code == "db.fake.fetch");
    CHECK(query.is_active());
    CHECK_FALSE(query.is_valid());

    fixture.state->next_error.reset();
    REQUIRE(query.finish());
    CHECK_FALSE(query.is_active());
    CHECK_FALSE(query.last_error());
}

TEST_CASE("Query finish retains preparation while clear discards it", "[db][query]")
{
    OpenDatabase fixture;
    Query        query{fixture.database};

    REQUIRE(query.exec("DELETE FROM jobs"));
    REQUIRE(query.finish());
    CHECK(query.last_query() == "DELETE FROM jobs");
    REQUIRE(query.exec());

    query.clear();
    CHECK_FALSE(query.is_active());
    CHECK(query.last_query().empty());
    CHECK_FALSE(query.last_error());
    CHECK(fixture.state->clear_count >= 1);

    auto executed = query.exec();
    REQUIRE_FALSE(executed);
    CHECK(executed.error().code == "db.invalid_query");
}

TEST_CASE("Query retains active state when backend finish fails", "[db][query]")
{
    OpenDatabase fixture;
    Query        query{fixture.database};
    REQUIRE(query.exec("DELETE FROM jobs"));

    fixture.state->finish_error = test_error("db.fake.finish");
    auto finished               = query.finish();
    REQUIRE_FALSE(finished);
    CHECK(finished.error().code == "db.fake.finish");
    CHECK(query.is_active());
    REQUIRE(query.last_error());
    CHECK(query.last_error()->code == "db.fake.finish");

    fixture.state->finish_error.reset();
    REQUIRE(query.finish());
    CHECK_FALSE(query.is_active());
    CHECK_FALSE(query.last_error());
}

TEST_CASE("Query stores and clears driver preparation and execution errors", "[db][query]")
{
    OpenDatabase fixture;
    Query        query{fixture.database};

    fixture.state->create_query_error = test_error("db.fake.create_query");
    auto created                      = query.prepare("SELECT 1");
    REQUIRE_FALSE(created);
    CHECK(created.error().code == "db.fake.create_query");

    fixture.state->create_query_error.reset();
    fixture.state->prepare_error = test_error("db.fake.prepare");
    auto prepared                = query.prepare("SELECT 1");
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error().code == "db.fake.prepare");
    CHECK(query.last_query().empty());

    fixture.state->prepare_error.reset();
    REQUIRE(query.prepare("SELECT 1"));
    CHECK_FALSE(query.last_error());

    fixture.state->exec_error = test_error("db.fake.exec");
    auto executed             = query.exec();
    REQUIRE_FALSE(executed);
    CHECK(executed.error().code == "db.fake.exec");
    CHECK_FALSE(query.is_active());
}

TEST_CASE("Query rejects other-thread operations before calling its driver", "[db][query][thread]")
{
    OpenDatabase fixture;
    Query        query{fixture.database};
    REQUIRE(query.prepare("SELECT 1"));
    auto const           calls_before = fixture.state->calls.size();
    std::optional<Error> thread_error;

    std::thread other_thread{[&] {
        auto executed = query.exec();
        if (!executed) {
            thread_error = executed.error();
        }
    }};
    other_thread.join();

    REQUIRE(thread_error);
    CHECK(thread_error->code == "db.wrong_thread");
    CHECK(fixture.state->calls.size() == calls_before);
    REQUIRE(query.exec());
}

TEST_CASE("Moving a Query transfers one database lifetime registration", "[db][query]")
{
    OpenDatabase fixture;
    {
        Query source{fixture.database};
        REQUIRE(source.prepare("SELECT 1"));
        Query moved{std::move(source)};
        CHECK(moved.last_query() == "SELECT 1");
        CHECK_FALSE(source.is_active());
        auto moved_from_exec = source.exec();
        REQUIRE_FALSE(moved_from_exec);
        CHECK(moved_from_exec.error().code == "db.invalid_query");

        auto close_with_query = fixture.database.close();
        REQUIRE_FALSE(close_with_query);
        CHECK(close_with_query.error().code == "db.query_active");
    }
    REQUIRE(fixture.database.close());
}
