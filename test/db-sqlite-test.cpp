#include "support/temporary_directory.hpp"

#include "database.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::db;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto make_database(std::filesystem::path     database_file,
                   std::chrono::milliseconds busy_timeout = 1000ms,
                   sqlite::Durability        durability   = sqlite::Durability::Normal) -> Database
{
    return Database{std::make_unique<sqlite::Driver>(sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = busy_timeout,
        .durability    = durability,
    })};
}

struct OpenDatabase {
    TemporaryDirectory    directory;
    std::filesystem::path database_file{directory.path() / "jobu.sqlite"};
    Database              database;

    explicit OpenDatabase(sqlite::Durability        durability = sqlite::Durability::Normal,
                          std::chrono::milliseconds timeout    = 1000ms)
        : database{make_database(database_file, timeout, durability)}
    {
        REQUIRE(database.open());
    }
};

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

auto scalar(Database& database, std::string_view sql) -> Value
{
    Query query{database};
    REQUIRE(query.exec(sql));
    REQUIRE(query.next().value());
    auto value = query.value(0);
    REQUIRE_FALSE(query.next().value());
    return value;
}

auto open_error(sqlite::Options options) -> Error
{
    Database database{std::make_unique<sqlite::Driver>(std::move(options))};
    auto     opened = database.open();
    REQUIRE_FALSE(opened);
    return opened.error();
}

} // anonymous namespace

TEST_CASE("SQLite driver creates and verifies a configured file database", "[db][sqlite][configuration]")
{
    OpenDatabase fixture{sqlite::Durability::Normal, 1250ms};

    CHECK(fixture.database.driver_name() == "sqlite");
    CHECK(std::filesystem::is_regular_file(fixture.database_file));
    CHECK(std::filesystem::is_regular_file(fixture.database_file.string() + ".lock"));
    CHECK(std::get<std::string>(scalar(fixture.database, "PRAGMA journal_mode")) == "wal");
    CHECK(std::get<std::int64_t>(scalar(fixture.database, "PRAGMA foreign_keys")) == 1);
    CHECK(std::get<std::int64_t>(scalar(fixture.database, "PRAGMA busy_timeout")) == 1250);
    CHECK(std::get<std::int64_t>(scalar(fixture.database, "PRAGMA synchronous")) == 1);

    REQUIRE(fixture.database.close());
    CHECK_FALSE(fixture.database.is_open());

    OpenDatabase durable{sqlite::Durability::Full};
    CHECK(std::get<std::int64_t>(scalar(durable.database, "PRAGMA synchronous")) == 2);
}

TEST_CASE("SQLite driver validates file and timeout options", "[db][sqlite][configuration]")
{
    TemporaryDirectory directory;

    CHECK(open_error({.database_file = {}}).code == "db.sqlite.invalid_database_file");
    CHECK(open_error({.database_file = ":memory:"}).code == "db.sqlite.invalid_database_file");
    CHECK(open_error({.database_file = "file:jobu.sqlite"}).code == "db.sqlite.invalid_database_file");
    CHECK(open_error({.database_file = directory.path() / "low.sqlite", .busy_timeout = -1ms}).code ==
          "db.sqlite.invalid_busy_timeout");
    CHECK(open_error({.database_file = directory.path() / "high.sqlite", .busy_timeout = 5001ms}).code ==
          "db.sqlite.invalid_busy_timeout");
    CHECK(open_error({.database_file = directory.path() / "missing" / "jobu.sqlite"}).code == "db.sqlite.open_failed");
    CHECK(open_error({.database_file = directory.path()}).code == "db.sqlite.open_failed");
    CHECK(open_error({.database_file = directory.path() / "durability.sqlite",
                      .durability    = static_cast<sqlite::Durability>(255)})
              .code == "db.sqlite.invalid_durability");

    auto zero_timeout = make_database(directory.path() / "zero.sqlite", 0ms);
    REQUIRE(zero_timeout.open());
    REQUIRE(zero_timeout.close());

    auto maximum_timeout = make_database(directory.path() / "maximum.sqlite", 5000ms);
    REQUIRE(maximum_timeout.open());
    REQUIRE(maximum_timeout.close());
}

TEST_CASE("SQLite adjacent lock rejects a second owner and permits later reopen", "[db][sqlite][locking]")
{
    TemporaryDirectory directory;
    auto const         database_file = directory.path() / "locked.sqlite";
    auto               first         = make_database(database_file);
    auto               second        = make_database(database_file);

    REQUIRE(first.open());
    auto second_open = second.open();
    REQUIRE_FALSE(second_open);
    CHECK(second_open.error().category == ErrorCategory::Conflict);
    CHECK(second_open.error().code == "db.sqlite.already_in_use");

    REQUIRE(first.close());
    REQUIRE(second.open());
    REQUIRE(second.close());
}

TEST_CASE("SQLite prepared queries round trip every generic value", "[db][sqlite][query]")
{
    OpenDatabase fixture;
    execute(fixture.database,
            "CREATE TABLE values_table ("
            "id INTEGER PRIMARY KEY, null_value, integer_value INTEGER NOT NULL, real_value REAL NOT NULL, "
            "text_value TEXT NOT NULL, blob_value BLOB NOT NULL)");

    auto const text = std::string{"embedded\0text", 13};
    auto const blob = ByteBuffer{std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}, std::byte{0x01}};
    {
        Query insert{fixture.database};
        REQUIRE(
            insert.prepare("INSERT INTO values_table(null_value, integer_value, real_value, text_value, blob_value) "
                           "VALUES (?, ?, ?, ?, ?)"));
        REQUIRE(insert.bind_value(0, Null{}));
        REQUIRE(insert.bind_value(1, std::int64_t{-9223372036854775807LL}));
        REQUIRE(insert.bind_value(2, 3.25));
        REQUIRE(insert.bind_value(3, make_text(text)));
        REQUIRE(insert.bind_value(4, make_blob(blob)));
        REQUIRE(insert.exec());
        CHECK(insert.num_rows_affected() == 1);
    }

    {
        Query insert{fixture.database};
        REQUIRE(
            insert.prepare("INSERT INTO values_table(null_value, integer_value, real_value, text_value, blob_value) "
                           "VALUES (:null_value, :integer_value, :real_value, :text_value, :blob_value)"));
        REQUIRE(insert.bind_value(":blob_value", ByteBuffer{}));
        REQUIRE(insert.bind_value(":text_value", make_text("")));
        REQUIRE(insert.bind_value(":real_value", -0.5));
        REQUIRE(insert.bind_value(":integer_value", std::int64_t{42}));
        REQUIRE(insert.bind_value(":null_value", Null{}));
        REQUIRE(insert.exec());
        CHECK(insert.num_rows_affected() == 1);
    }

    Query select{fixture.database};
    REQUIRE(select.exec(
        "SELECT id, null_value, integer_value, real_value, text_value, blob_value FROM values_table ORDER BY id"));
    CHECK(select.record().count() == 6);
    CHECK(select.record().field_name(0) == "id");
    CHECK(select.record().is_null("integer_value"));

    REQUIRE(select.next().value());
    auto const* id = select.value("id");
    REQUIRE(id);
    CHECK(std::get<std::int64_t>(*id) == 1);
    CHECK(select.is_null("null_value"));
    CHECK(std::get<std::int64_t>(select.value(2)) == -9223372036854775807LL);
    CHECK(std::get<double>(select.value(3)) == 3.25);
    CHECK(std::get<std::string>(select.value(4)) == text);
    CHECK(std::get<ByteBuffer>(select.value(5)) == blob);

    REQUIRE(select.next().value());
    CHECK(std::get<std::int64_t>(select.value(2)) == 42);
    CHECK(std::get<std::string>(select.value(4)).empty());
    CHECK(std::get<ByteBuffer>(select.value(5)).empty());
    CHECK_FALSE(select.next().value());
    CHECK(select.record().count() == 6);

    REQUIRE(select.finish());
    Query update{fixture.database};
    REQUIRE(update.exec("UPDATE values_table SET integer_value = integer_value + 1"));
    CHECK(update.num_rows_affected() == 2);
}

TEST_CASE("SQLite maps preparation unique and foreign-key failures", "[db][sqlite][error]")
{
    OpenDatabase fixture;
    execute(fixture.database, "CREATE TABLE parent (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
    execute(fixture.database,
            "CREATE TABLE child (id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL REFERENCES parent(id))");
    execute(fixture.database, "INSERT INTO parent(id, name) VALUES (1, 'one')");

    {
        Query duplicate{fixture.database};
        auto  inserted = duplicate.exec("INSERT INTO parent(id, name) VALUES (2, 'one')");
        REQUIRE_FALSE(inserted);
        CHECK(inserted.error().category == ErrorCategory::Conflict);
        CHECK(inserted.error().code == "db.constraint.unique");
    }
    {
        Query foreign_key{fixture.database};
        auto  inserted = foreign_key.exec("INSERT INTO child(id, parent_id) VALUES (1, 999)");
        REQUIRE_FALSE(inserted);
        CHECK(inserted.error().category == ErrorCategory::Conflict);
        CHECK(inserted.error().code == "db.constraint.foreign_key");
    }
    {
        Query invalid{fixture.database};
        auto  prepared = invalid.prepare("SELECT FROM");
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code == "db.prepare_failed");
        CHECK(prepared.error().detail.find("SELECT FROM") == std::string::npos);
    }
    {
        Query multiple{fixture.database};
        auto  prepared = multiple.prepare("SELECT 1; SELECT 2");
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code == "db.prepare_failed");
    }
}

TEST_CASE("SQLite direct and guarded transactions commit or roll back", "[db][sqlite][transaction]")
{
    OpenDatabase fixture;
    execute(fixture.database, "CREATE TABLE transactions (id INTEGER PRIMARY KEY, value TEXT NOT NULL)");

    REQUIRE(fixture.database.transaction(TransactionMode::Immediate));
    execute(fixture.database, "INSERT INTO transactions VALUES (1, 'committed')");
    REQUIRE(fixture.database.commit());

    REQUIRE(fixture.database.transaction(TransactionMode::Deferred));
    execute(fixture.database, "INSERT INTO transactions VALUES (2, 'rolled back')");
    REQUIRE(fixture.database.rollback());

    {
        auto begun = Transaction::begin(fixture.database, TransactionMode::Exclusive);
        REQUIRE(begun);
        auto guard = std::move(begun).value();
        execute(fixture.database, "INSERT INTO transactions VALUES (3, 'scope rollback')");
    }

    CHECK(std::get<std::int64_t>(scalar(fixture.database, "SELECT count(*) FROM transactions")) == 1);
    CHECK(std::get<std::string>(scalar(fixture.database, "SELECT value FROM transactions WHERE id = 1")) ==
          "committed");
}

TEST_CASE("SQLite database destruction rolls back and releases ownership", "[db][sqlite][transaction][locking]")
{
    TemporaryDirectory directory;
    auto const         database_file = directory.path() / "destructor.sqlite";
    {
        auto database = make_database(database_file);
        REQUIRE(database.open());
        execute(database, "CREATE TABLE pending (id INTEGER PRIMARY KEY)");
        REQUIRE(database.transaction());
        execute(database, "INSERT INTO pending VALUES (1)");
    }

    auto reopened = make_database(database_file);
    REQUIRE(reopened.open());
    CHECK(std::get<std::int64_t>(scalar(reopened, "SELECT count(*) FROM pending")) == 0);
    REQUIRE(reopened.close());
}

TEST_CASE("SQLite rejects corrupt database content and releases its failed-open lock", "[db][sqlite][error]")
{
    TemporaryDirectory directory;
    auto const         database_file = directory.path() / "corrupt.sqlite";
    {
        std::ofstream output{database_file, std::ios::binary};
        REQUIRE(output);
        output << "this is not a sqlite database";
    }

    auto first  = make_database(database_file);
    auto opened = first.open();
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == "db.corrupt");

    auto second  = make_database(database_file);
    auto retried = second.open();
    REQUIRE_FALSE(retried);
    CHECK(retried.error().code == "db.corrupt");
}

TEST_CASE("SQLite artifacts stay contained in TemporaryDirectory cleanup", "[db][sqlite][filesystem]")
{
    TemporaryDirectory directory;
    auto const         directory_path = directory.path();
    {
        auto database = make_database(directory.path() / "cleanup.sqlite");
        REQUIRE(database.open());
        execute(database, "CREATE TABLE cleanup (id INTEGER PRIMARY KEY)");
        execute(database, "INSERT INTO cleanup VALUES (1)");
        REQUIRE(database.close());
    }

    REQUIRE_FALSE(directory.cleanup());
    CHECK_FALSE(std::filesystem::exists(directory_path));
}
