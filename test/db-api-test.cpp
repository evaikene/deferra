#include "support/fake_database_driver.hpp"

#include "database.hpp"
#include "query.hpp"
#include "record.hpp"
#include "value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

using namespace jb::core;
using namespace jb::db;
using namespace jb::test;

TEST_CASE("Database values preserve type and bytes", "[db][value]")
{
    auto const text = make_text(std::string_view{"text\0value", 10});
    CHECK(std::get<std::string>(text) == std::string{"text\0value", 10});

    ByteBuffer bytes{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
    auto const blob = make_blob(bytes);
    CHECK(std::get<ByteBuffer>(blob) == bytes);

    CHECK(std::holds_alternative<Null>(Value{Null{}}));
    CHECK(std::holds_alternative<std::int64_t>(Value{std::int64_t{42}}));
    CHECK(std::holds_alternative<double>(Value{1.5}));
    CHECK(std::holds_alternative<std::string>(text));
    CHECK(std::holds_alternative<ByteBuffer>(blob));
}

TEST_CASE("Fields expose owning values and SQL null state", "[db][record]")
{
    Field value_field{"name", make_text("JobU")};
    CHECK(value_field.name() == "name");
    CHECK(std::get<std::string>(value_field.value()) == "JobU");
    CHECK_FALSE(value_field.is_null());

    Field null_field{"optional", Null{}};
    CHECK(null_field.is_null());
}

TEST_CASE("Records support indexed and ASCII case-insensitive named access", "[db][record]")
{
    Record record{
        {
         Field{"ID", std::int64_t{7}},
         Field{"Name", make_text("first")},
         Field{"name", make_text("second")},
         Field{"optional", Null{}},
         }
    };

    CHECK(record.count() == 4);
    CHECK_FALSE(record.is_empty());
    CHECK(record.contains("id"));
    CHECK(record.index_of("nAmE") == 1);
    CHECK(record.index_of("missing") == -1);
    CHECK(record.field_name(0) == "ID");
    CHECK(std::get<std::int64_t>(record.value(0)) == 7);
    REQUIRE(record.value("NAME") != nullptr);
    CHECK(std::get<std::string>(*record.value("NAME")) == "first");
    CHECK(record.value("missing") == nullptr);
    CHECK(record.is_null(3));
    CHECK(record.is_null(10));
    CHECK(record.is_null("OPTIONAL"));
    CHECK(record.is_null("missing"));
    CHECK_FALSE(record.is_null("id"));

    CHECK_THROWS_AS(record.field(10), std::out_of_range);
    CHECK_THROWS_AS(record.field_name(10), std::out_of_range);
    CHECK_THROWS_AS(record.value(10), std::out_of_range);
}

TEST_CASE("Record is an ordinary owning copyable value", "[db][record]")
{
    Record original{{Field{"value", make_text("owned")}}};
    auto   copy = original;

    CHECK(std::get<std::string>(copy.value(0)) == "owned");
}

TEST_CASE("Fake driver scaffolding exposes a backend contract", "[db][driver]")
{
    auto               state = std::make_shared<FakeDatabaseDriverState>();
    FakeDatabaseDriver driver{state};

    CHECK(driver.name() == "fake");
    CHECK_FALSE(state->open);
    CHECK(state->calls.empty());
}

TEST_CASE("Default Database is invalid and stores generic errors", "[db][database]")
{
    Database database;

    CHECK_FALSE(database.is_valid());
    CHECK_FALSE(database.is_open());
    CHECK(database.driver_name().empty());
    CHECK_FALSE(database.last_error());

    auto opened = database.open();
    REQUIRE_FALSE(opened);
    CHECK(opened.error().category == ErrorCategory::InvalidArgument);
    CHECK(opened.error().code == "db.invalid_database");
    REQUIRE(database.last_error());
    CHECK(database.last_error()->code == "db.invalid_database");
}

TEST_CASE("Database owns driver open close and name behavior", "[db][database]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database database{std::make_unique<FakeDatabaseDriver>(state)};

    CHECK(database.is_valid());
    CHECK_FALSE(database.is_open());
    CHECK(database.driver_name() == "fake");

    REQUIRE(database.open());
    CHECK(database.is_open());
    CHECK(state->open);
    REQUIRE(database.open());
    CHECK(std::count(state->calls.begin(), state->calls.end(), "driver.open") == 1);

    REQUIRE(database.close());
    CHECK_FALSE(database.is_open());
    CHECK_FALSE(state->open);
    REQUIRE(database.close());
    CHECK(std::count(state->calls.begin(), state->calls.end(), "driver.close") == 1);
}

TEST_CASE("Database propagates driver errors and clears old errors on success", "[db][database]")
{
    auto state        = std::make_shared<FakeDatabaseDriverState>();
    state->open_error = Error{
        .category = ErrorCategory::Io,
        .code     = "db.fake.open",
        .message  = "Injected open failure",
    };
    Database database{std::make_unique<FakeDatabaseDriver>(state)};

    auto opened = database.open();
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == "db.fake.open");
    REQUIRE(database.last_error());
    CHECK(database.last_error()->code == "db.fake.open");

    state->open_error.reset();
    REQUIRE(database.open());
    CHECK_FALSE(database.last_error());

    state->close_error = Error{
        .category = ErrorCategory::Io,
        .code     = "db.fake.close",
        .message  = "Injected close failure",
    };
    auto closed = database.close();
    REQUIRE_FALSE(closed);
    CHECK(closed.error().code == "db.fake.close");
    CHECK(database.is_open());

    state->close_error.reset();
    REQUIRE(database.close());
    CHECK_FALSE(database.last_error());
}

TEST_CASE("Database rejects other-thread operations before calling its driver", "[db][database][thread]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database database{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(database.open());
    auto const           calls_before = state->calls.size();
    std::optional<Error> thread_error;

    std::thread other_thread{[&] {
        auto closed = database.close();
        if (!closed) {
            thread_error = closed.error();
        }
    }};
    other_thread.join();

    REQUIRE(thread_error);
    CHECK(thread_error->code == "db.wrong_thread");
    CHECK(state->calls.size() == calls_before);
    CHECK(database.is_open());
    REQUIRE(database.close());
}

TEST_CASE("Closed Database may reopen on another thread", "[db][database][thread]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database database{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(database.open());
    REQUIRE(database.close());
    std::optional<Error> thread_error;
    bool                 opened_on_thread{false};

    std::thread other_thread{[&] {
        auto opened = database.open();
        if (!opened) {
            thread_error = opened.error();
            return;
        }
        opened_on_thread = database.is_open();
        auto closed      = database.close();
        if (!closed) {
            thread_error = closed.error();
        }
    }};
    other_thread.join();

    CHECK_FALSE(thread_error);
    CHECK(opened_on_thread);
    CHECK_FALSE(database.is_open());
    CHECK(std::count(state->calls.begin(), state->calls.end(), "driver.open") == 2);
    CHECK(std::count(state->calls.begin(), state->calls.end(), "driver.close") == 2);
}

TEST_CASE("Database close waits for every Query lifetime", "[db][database][query]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database database{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(database.open());

    {
        Query query{database};
        auto  closed = database.close();
        REQUIRE_FALSE(closed);
        CHECK(closed.error().code == "db.query_active");
        CHECK(database.is_open());
    }

    REQUIRE(database.close());
}

TEST_CASE("Database move construction transfers an idle connection", "[db][database]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database source{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(source.open());

    Database moved{std::move(source)};
    CHECK_FALSE(source.is_valid());
    CHECK(moved.is_valid());
    CHECK(moved.is_open());
    CHECK(moved.driver_name() == "fake");
    REQUIRE(moved.close());
}

TEST_CASE("Database move construction rejects a live Query without invalidating its source", "[db][database][query]")
{
    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database source{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(source.open());

    {
        Query    query{source};
        Database moved{std::move(source)};
        CHECK_FALSE(moved.is_valid());
        CHECK(source.is_valid());
        CHECK(source.is_open());
        REQUIRE(source.last_error());
        CHECK(source.last_error()->code == "db.query_active");
    }

    REQUIRE(source.close());
}
