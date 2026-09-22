#include "sqlite/sqlite_schema.hpp"

#include "database.hpp"
#include "query.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema_priv.hpp"
#include "support/fake_database_driver.hpp"
#include "support/fault_database_driver.hpp"
#include "support/sqlite_schema_fixture.hpp"
#include "support/storage_fault_helpers.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::db;
using namespace jb::test;
using namespace std::chrono_literals;

namespace jobu_sqlite = jb::jobu::sqlite;

namespace {

auto make_database(std::filesystem::path database_file) -> Database
{
    return Database{std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
        .database_file = std::move(database_file),
        .busy_timeout  = 1000ms,
        .durability    = jb::db::sqlite::Durability::Normal,
    })};
}

struct OpenDatabase {
    TemporaryDirectory    directory;
    std::filesystem::path database_file{directory.path() / "jobu.sqlite"};
    Database              database{make_database(database_file)};

    OpenDatabase() { REQUIRE(database.open()); }
};

void execute(Database& database, std::string_view sql)
{
    Query query{database};
    REQUIRE(query.exec(sql));
}

auto execute_error(Database& database, std::string_view sql) -> Error
{
    Query query{database};
    auto  executed = query.exec(sql);
    REQUIRE_FALSE(executed);
    return std::move(executed).error();
}

auto scalar_integer(Database& database, std::string_view sql) -> std::int64_t
{
    Query query{database};
    REQUIRE(query.exec(sql));
    auto next = query.next();
    REQUIRE(next);
    REQUIRE(next.value());
    REQUIRE(query.record().count() == 1);
    auto const* value = std::get_if<std::int64_t>(&query.value(0));
    REQUIRE(value);
    auto result = *value;
    next        = query.next();
    REQUIRE(next);
    REQUIRE_FALSE(next.value());
    return result;
}

auto require_schema_error(Result<jobu_sqlite::SchemaStatus, Error> const& result,
                          ErrorCategory                                   category,
                          std::string_view                                code) -> Error
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    CHECK(result.error().code == code);
    return result.error();
}

void insert_queue(Database& database)
{
    execute(database, R"sql(
INSERT INTO jobu_queues(
    id, name, deleted_name, state, weight, concurrency_limit, recovery_policy, defaults_json,
    retention_seconds, runnable_wait_warning_ms, created_at_us, updated_at_us, deleted_at_us
) VALUES (
    X'000102030405060708090A0B0C0D0E0F', 'default', NULL, 'active', 1, 1, 'fail_interrupted',
    '{"version":1,"values":{}}', NULL, 10000, 0, 0, NULL
))sql");
}

void insert_job(Database& database)
{
    execute(database, R"sql(
INSERT INTO jobu_jobs(
    id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, cron_expression, cron_timezone,
    priority, attributes_json, payload_json, created_at_us, updated_at_us, deleted_at_us
) VALUES (
    X'202122232425262728292A2B2C2D2E2F', X'000102030405060708090A0B0C0D0E0F', 1, 'example', 'active',
    'cli', 'once', 10, NULL, NULL, 0, '{"version":1,"values":{}}', '{"command":"/true"}', 0, 0, NULL
))sql");
}

void insert_run(Database& database)
{
    execute(database, R"sql(
INSERT INTO jobu_runs(
    id, job_id, job_revision, queue_id, origin, schedule_owned, planned_at_us, runnable_at_us,
    started_at_us, completed_at_us, type, priority, attributes_json, payload_json, state, result_json
) VALUES (
    X'303132333435363738393A3B3C3D3E3F', X'202122232425262728292A2B2C2D2E2F', 1,
    X'000102030405060708090A0B0C0D0E0F', 'scheduled', 1, 10, 10, NULL, NULL, 'cli', 0,
    '{"version":1,"values":{}}', '{"command":"/true"}', 'scheduled', NULL
))sql");
}

// Every mutable table is populated so an index-only upgrade cannot silently rewrite or lose application state.
void populate_version_one(Database& database)
{
    create_version_one_schema(database);
    insert_queue(database);
    insert_job(database);
    insert_run(database);
    execute(database, R"sql(INSERT INTO jobu_attempts VALUES (
        X'303132333435363738393A3B3C3D3E3F', 1, 10, NULL, NULL, 'pending', NULL, NULL))sql");
    execute(database, R"sql(INSERT INTO jobu_attempt_output VALUES (
        X'303132333435363738393A3B3C3D3E3F', 1, X'00FF01', X'', 1, 0, 0))sql");
    execute(database, "INSERT INTO jobu_secrets VALUES ('token', X'00FF010A', 1, 2)");
    execute(database, R"sql(INSERT INTO jobu_secret_refs VALUES (
        'token', X'202122232425262728292A2B2C2D2E2F', '/environment/TOKEN'))sql");
    execute(database, R"sql(INSERT INTO jobu_idempotency VALUES (
        'job.create', X'000102030405060708090A0B0C0D0E0F', 'key', '{"request":1}', '{"result":2}',
        X'202122232425262728292A2B2C2D2E2F', 3, NULL))sql");
}

struct UpgradeFixture {
    TemporaryDirectory                  directory;
    std::filesystem::path               database_file{directory.path() / "upgrade.sqlite"};
    std::shared_ptr<DatabaseFaultState> faults{std::make_shared<DatabaseFaultState>()};
    Database                            database{std::make_unique<FaultDatabaseDriver>(
        std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{.database_file = database_file}),
        faults)};
    StorageSnapshot                     before;

    UpgradeFixture()
    {
        REQUIRE(database.open());
        populate_version_one(database);
        before           = storage_snapshot(database);
        faults->classify = [](std::string_view sql) -> std::string {
            if (sql.starts_with("CREATE INDEX jobu_runs_planned_id_idx")) {
                return "index.planned";
            }
            if (sql.starts_with("CREATE INDEX jobu_runs_queue_planned_id_idx")) {
                return "index.queue";
            }
            if (sql.starts_with("CREATE INDEX jobu_runs_job_planned_id_idx")) {
                return "index.job";
            }
            if (sql.starts_with("UPDATE jobu_schema SET version")) {
                return "marker";
            }
            return "other";
        };
        faults->calls.clear();
    }

    void require_unchanged()
    {
        REQUIRE_FALSE(database.is_poisoned());
        CHECK(scalar_integer(database, "SELECT version FROM jobu_schema") == 1);
        CHECK(scalar_integer(database,
                             "SELECT count(*) FROM sqlite_schema WHERE name LIKE 'jobu_runs%planned_id_idx'") == 1);
        CHECK(storage_snapshot(database) == before);
    }
};

auto fail_after_first_creation(std::size_t completed_statements, std::string_view /*statement*/) -> Result<void, Error>
{
    if (completed_statements == 1) {
        return Result<void, Error>::failure({
            .category = ErrorCategory::Internal,
            .code     = "test.schema.injected_failure",
            .message  = "Injected schema creation failure",
        });
    }
    return Result<void, Error>::success();
}

} // anonymous namespace

TEST_CASE("JobU SQLite schema rejects invalid database preconditions", "[jobu][sqlite][schema]")
{
    Database invalid;
    require_schema_error(jobu_sqlite::ensure_schema(invalid),
                         ErrorCategory::InvalidArgument,
                         "jobu.schema.invalid_database");

    TemporaryDirectory directory;
    auto               closed = make_database(directory.path() / "closed.sqlite");
    require_schema_error(jobu_sqlite::ensure_schema(closed),
                         ErrorCategory::InvalidArgument,
                         "jobu.schema.invalid_database");

    auto     state = std::make_shared<FakeDatabaseDriverState>();
    Database fake{std::make_unique<FakeDatabaseDriver>(state)};
    REQUIRE(fake.open());
    require_schema_error(jobu_sqlite::ensure_schema(fake),
                         ErrorCategory::InvalidArgument,
                         "jobu.schema.invalid_database");
    REQUIRE(fake.close());

    auto open = make_database(directory.path() / "active-transaction.sqlite");
    REQUIRE(open.open());
    REQUIRE(open.transaction());
    auto transaction_error = require_schema_error(jobu_sqlite::ensure_schema(open),
                                                  ErrorCategory::InvalidArgument,
                                                  "jobu.schema.invalid_database");
    CHECK(transaction_error.detail.find("lower_code=db.transaction_active") != std::string::npos);
    CHECK(transaction_error.detail.find("BEGIN") == std::string::npos);
    REQUIRE(open.rollback());
    REQUIRE(open.close());
}

TEST_CASE("JobU SQLite schema creates validates and reopens version two", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;

    auto created = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(created);
    CHECK(created->version == jobu_sqlite::current_schema_version);
    CHECK(created->created);
    CHECK_FALSE(created->upgraded);

    auto const manifest = jobu_sqlite::detail::schema_object_manifest();
    CHECK(manifest.size() == 21);
    CHECK(std::ranges::count_if(manifest, [](auto const& object) {
              return object.kind == jobu_sqlite::detail::SchemaObjectKind::Table;
          }) == 9);
    CHECK(std::ranges::count_if(manifest, [](auto const& object) {
              return object.kind == jobu_sqlite::detail::SchemaObjectKind::Index;
          }) == 12);

    for (auto const& object : manifest) {
        Query query{fixture.database};
        REQUIRE(query.prepare("SELECT type, tbl_name FROM sqlite_schema WHERE name = :name"));
        REQUIRE(query.bind_value(":name", make_text(object.name)));
        REQUIRE(query.exec());
        auto next = query.next();
        REQUIRE(next);
        REQUIRE(next.value());
        REQUIRE(query.record().count() == 2);
        auto const* type  = std::get_if<std::string>(&query.value(0));
        auto const* owner = std::get_if<std::string>(&query.value(1));
        REQUIRE(type);
        REQUIRE(owner);
        CHECK(*type == (object.kind == jobu_sqlite::detail::SchemaObjectKind::Table ? "table" : "index"));
        CHECK(*owner == object.owner);
        next = query.next();
        REQUIRE(next);
        REQUIRE_FALSE(next.value());

        if (!object.column_probe.empty()) {
            Query probe{fixture.database};
            REQUIRE(probe.exec(object.column_probe));
            auto row = probe.next();
            REQUIRE(row);
            CHECK_FALSE(row.value());
        }
    }

    CHECK(scalar_integer(fixture.database, "SELECT singleton FROM jobu_schema") == 1);
    CHECK(scalar_integer(fixture.database, "SELECT version FROM jobu_schema") == 2);
    CHECK(scalar_integer(fixture.database, "PRAGMA foreign_keys") == 1);

    auto repeated = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(repeated);
    CHECK(repeated->version == 2);
    CHECK_FALSE(repeated->created);
    CHECK_FALSE(repeated->upgraded);

    REQUIRE(fixture.database.close());
    auto reopened = make_database(fixture.database_file);
    REQUIRE(reopened.open());
    auto validated = jobu_sqlite::ensure_schema(reopened);
    REQUIRE(validated);
    CHECK(validated->version == 2);
    CHECK_FALSE(validated->created);
    CHECK_FALSE(validated->upgraded);
    REQUIRE(reopened.close());
}

TEST_CASE("JobU SQLite schema enforces durable constraints and cascades", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;
    REQUIRE(jobu_sqlite::ensure_schema(fixture.database));

    insert_queue(fixture.database);
    CHECK(execute_error(fixture.database, R"sql(
INSERT INTO jobu_queues VALUES (
    X'101112131415161718191A1B1C1D1E1F', 'default', NULL, 'active', 1, 1, 'fail_interrupted', '{}',
    NULL, 10000, 0, 0, NULL
))sql")
              .code == "db.constraint.unique");
    CHECK(execute_error(fixture.database, R"sql(
INSERT INTO jobu_queues VALUES (
    X'01', 'short-id', NULL, 'active', 1, 1, 'fail_interrupted', '{}', NULL, 10000, 0, 0, NULL
))sql")
              .code == "db.constraint");
    CHECK(execute_error(fixture.database, "UPDATE jobu_queues SET state = 'unknown' WHERE name = 'default'").code ==
          "db.constraint");
    CHECK(execute_error(fixture.database, "UPDATE jobu_queues SET weight = 0 WHERE name = 'default'").code ==
          "db.constraint");
    CHECK(execute_error(fixture.database, "UPDATE jobu_queues SET state = 'deleted' WHERE name = 'default'").code ==
          "db.constraint");

    insert_job(fixture.database);
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_jobs SET queue_id = X'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' WHERE name = 'example'")
              .code == "db.constraint.foreign_key");
    CHECK(execute_error(fixture.database, "UPDATE jobu_jobs SET revision = 0 WHERE name = 'example'").code ==
          "db.constraint");
    CHECK(execute_error(fixture.database, "UPDATE jobu_jobs SET priority = 2147483648 WHERE name = 'example'").code ==
          "db.constraint");
    CHECK(execute_error(fixture.database, "UPDATE jobu_jobs SET schedule_kind = 'cron' WHERE name = 'example'").code ==
          "db.constraint");

    insert_run(fixture.database);
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_runs SET schedule_owned = 2 WHERE id = X'303132333435363738393A3B3C3D3E3F'")
              .code == "db.constraint");
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_runs SET state = 'unknown' WHERE id = X'303132333435363738393A3B3C3D3E3F'")
              .code == "db.constraint");
    CHECK(execute_error(fixture.database, R"sql(
INSERT INTO jobu_runs VALUES (
    X'404142434445464748494A4B4C4D4E4F', X'202122232425262728292A2B2C2D2E2F', 1,
    X'000102030405060708090A0B0C0D0E0F', 'scheduled', 1, 20, 20, NULL, NULL, 'cli', 0, '{}', '{}',
    'scheduled', NULL
))sql")
              .code == "db.constraint.unique");
    execute(fixture.database, R"sql(
INSERT INTO jobu_runs VALUES (
    X'404142434445464748494A4B4C4D4E4F', X'202122232425262728292A2B2C2D2E2F', 1,
    X'000102030405060708090A0B0C0D0E0F', 'scheduled', 1, 20, 20, 20, 20, 'cli', 0, '{}', '{}',
    'succeeded', '{}'
))sql");
    execute(fixture.database, "DELETE FROM jobu_runs WHERE id = X'404142434445464748494A4B4C4D4E4F'");

    execute(fixture.database, R"sql(
INSERT INTO jobu_attempts VALUES (
    X'303132333435363738393A3B3C3D3E3F', 1, 10, NULL, NULL, 'pending', NULL, NULL
))sql");
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_attempts SET attempt_number = 0 WHERE run_id = "
                        "X'303132333435363738393A3B3C3D3E3F'")
              .code == "db.constraint");
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_attempts SET outcome = 'unknown' WHERE run_id = "
                        "X'303132333435363738393A3B3C3D3E3F'")
              .code == "db.constraint");

    execute(fixture.database, R"sql(
INSERT INTO jobu_attempt_output VALUES (
    X'303132333435363738393A3B3C3D3E3F', 1, X'0001', NULL, 0, 0, 0
))sql");
    CHECK(execute_error(fixture.database,
                        "UPDATE jobu_attempt_output SET capture_lost = 2 WHERE run_id = "
                        "X'303132333435363738393A3B3C3D3E3F'")
              .code == "db.constraint");

    execute(fixture.database, "INSERT INTO jobu_secrets VALUES ('token', X'0001', 0, 0)");
    CHECK(execute_error(fixture.database, "UPDATE jobu_secrets SET value_blob = 'text' WHERE name = 'token'").code ==
          "db.constraint");
    execute(fixture.database, R"sql(
INSERT INTO jobu_secret_refs VALUES (
    'token', X'202122232425262728292A2B2C2D2E2F', 'payload.token'
))sql");
    CHECK(execute_error(fixture.database, R"sql(
INSERT INTO jobu_secret_refs VALUES (
    'missing', X'202122232425262728292A2B2C2D2E2F', 'payload.other'
))sql")
              .code == "db.constraint.foreign_key");

    execute(fixture.database, R"sql(
INSERT INTO jobu_idempotency VALUES (
    'job.create', X'000102030405060708090A0B0C0D0E0F', 'key', '{}', '{}',
    X'202122232425262728292A2B2C2D2E2F', 0, NULL
))sql");
    CHECK(execute_error(fixture.database, "UPDATE jobu_idempotency SET scope_id = X'00' WHERE method = 'job.create'")
              .code == "db.constraint");

    execute(fixture.database, "DELETE FROM jobu_runs WHERE id = X'303132333435363738393A3B3C3D3E3F'");
    CHECK(scalar_integer(fixture.database, "SELECT count(*) FROM jobu_attempts") == 0);
    CHECK(scalar_integer(fixture.database, "SELECT count(*) FROM jobu_attempt_output") == 0);
    execute(fixture.database, "DELETE FROM jobu_jobs WHERE id = X'202122232425262728292A2B2C2D2E2F'");
    CHECK(scalar_integer(fixture.database, "SELECT count(*) FROM jobu_secret_refs") == 0);
    REQUIRE(jobu_sqlite::ensure_schema(fixture.database));
}

TEST_CASE("JobU SQLite schema does not claim an unmarked user database", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;

    SECTION("table")
    {
        execute(fixture.database, "CREATE TABLE user_table (id INTEGER)");
    }
    SECTION("index")
    {
        execute(fixture.database, "CREATE TABLE user_table (id INTEGER)");
        execute(fixture.database, "CREATE INDEX user_index ON user_table(id)");
    }
    SECTION("trigger")
    {
        execute(fixture.database, "CREATE TABLE user_table (id INTEGER)");
        execute(fixture.database,
                "CREATE TRIGGER user_trigger AFTER INSERT ON user_table BEGIN UPDATE user_table SET id = id; END");
    }
    SECTION("view")
    {
        execute(fixture.database, "CREATE VIEW user_view AS SELECT 1 AS value");
    }

    auto error = require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                                      ErrorCategory::Conflict,
                                      "jobu.schema.database_not_empty");
    CHECK(error.detail.find("context=") != std::string::npos);
    CHECK(scalar_integer(fixture.database, "SELECT count(*) FROM sqlite_schema WHERE name = 'jobu_schema'") == 0);
}

TEST_CASE("JobU SQLite schema rejects malformed and newer markers", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;

    SECTION("missing row")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
    }
    SECTION("duplicate row")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
        execute(fixture.database, "INSERT INTO jobu_schema VALUES (1, 1), (1, 1)");
    }
    SECTION("zero version")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
        execute(fixture.database, "INSERT INTO jobu_schema VALUES (1, 0)");
    }
    SECTION("negative version")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
        execute(fixture.database, "INSERT INTO jobu_schema VALUES (1, -1)");
    }
    SECTION("text version")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
        execute(fixture.database, "INSERT INTO jobu_schema VALUES (1, 'one')");
    }
    SECTION("wrong singleton")
    {
        execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
        execute(fixture.database, "INSERT INTO jobu_schema VALUES (2, 1)");
    }
    SECTION("wrong object kind")
    {
        execute(fixture.database, "CREATE VIEW jobu_schema AS SELECT 1 AS version");
    }

    auto error = require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                                      ErrorCategory::Internal,
                                      "jobu.schema.invalid");
    CHECK(error.detail.find("SELECT") == std::string::npos);
}

TEST_CASE("JobU SQLite schema rejects a newer marker", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;
    execute(fixture.database, "CREATE TABLE jobu_schema (singleton, version)");
    execute(fixture.database, "INSERT INTO jobu_schema VALUES (1, 3)");
    require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                         ErrorCategory::Unsupported,
                         "jobu.schema.newer_database");
}

TEST_CASE("JobU SQLite schema detects missing required objects and columns", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;
    REQUIRE(jobu_sqlite::ensure_schema(fixture.database));

    SECTION("missing table")
    {
        execute(fixture.database, "DROP TABLE jobu_attempt_output");
    }
    SECTION("missing column")
    {
        execute(fixture.database, "ALTER TABLE jobu_queues RENAME COLUMN defaults_json TO absent_defaults_json");
    }
    SECTION("missing partial index")
    {
        execute(fixture.database, "DROP INDEX jobu_runs_schedule_owned_non_terminal_uidx");
    }

    auto error = require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                                      ErrorCategory::Internal,
                                      "jobu.schema.invalid");
    CHECK(error.detail.find("SELECT") == std::string::npos);
    CHECK(error.detail.find("CREATE") == std::string::npos);
}

TEST_CASE("JobU SQLite schema verifies the foreign key setting and existing rows", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;
    REQUIRE(jobu_sqlite::ensure_schema(fixture.database));

    SECTION("disabled setting")
    {
        execute(fixture.database, "PRAGMA foreign_keys = OFF");
        CHECK(scalar_integer(fixture.database, "PRAGMA foreign_keys") == 0);
        require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                             ErrorCategory::Internal,
                             "jobu.schema.invalid");
        execute(fixture.database, "PRAGMA foreign_keys = ON");
    }

    SECTION("foreign key violation")
    {
        execute(fixture.database, "PRAGMA foreign_keys = OFF");
        execute(fixture.database, R"sql(
INSERT INTO jobu_jobs(
    id, queue_id, revision, name, state, type, schedule_kind, scheduled_at_us, cron_expression, cron_timezone,
    priority, attributes_json, payload_json, created_at_us, updated_at_us, deleted_at_us
) VALUES (
    X'202122232425262728292A2B2C2D2E2F', X'000102030405060708090A0B0C0D0E0F', 1, NULL, 'active',
    'cli', 'once', 10, NULL, NULL, 0, '{}', '{}', 0, 0, NULL
))sql");
        execute(fixture.database, "PRAGMA foreign_keys = ON");
        CHECK(scalar_integer(fixture.database, "PRAGMA foreign_keys") == 1);
        require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                             ErrorCategory::Internal,
                             "jobu.schema.invalid");
    }
}

TEST_CASE("JobU SQLite schema rolls back injected partial creation", "[jobu][sqlite][schema]")
{
    OpenDatabase fixture;

    auto failed = jobu_sqlite::detail::ensure_schema_impl(fixture.database, fail_after_first_creation);
    auto error  = require_schema_error(failed, ErrorCategory::Internal, "jobu.schema.create_failed");
    CHECK(error.detail.find("lower_code=test.schema.injected_failure") != std::string::npos);
    CHECK(error.detail.find("CREATE TABLE") == std::string::npos);
    CHECK(scalar_integer(fixture.database, "SELECT count(*) FROM sqlite_schema WHERE name GLOB 'jobu_*'") == 0);

    auto created = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(created);
    CHECK(created->created);
    CHECK_FALSE(created->upgraded);
}

TEST_CASE("JobU SQLite schema upgrades populated version one atomically", "[jobu][sqlite][schema]")
{
    UpgradeFixture fixture;
    REQUIRE(jobu_sqlite::detail::schema_v1_object_manifest().size() == 18);
    fixture.require_unchanged();

    auto upgraded = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(upgraded);
    CHECK(upgraded->version == 2);
    CHECK_FALSE(upgraded->created);
    CHECK(upgraded->upgraded);
    CHECK(storage_snapshot(fixture.database) == fixture.before);
    CHECK(scalar_integer(fixture.database, "SELECT version FROM jobu_schema") == 2);

    auto repeated = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(repeated);
    CHECK_FALSE(repeated->created);
    CHECK_FALSE(repeated->upgraded);
    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    auto reopened = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(reopened);
    CHECK_FALSE(reopened->created);
    CHECK_FALSE(reopened->upgraded);
    CHECK(storage_snapshot(fixture.database) == fixture.before);
}

TEST_CASE("JobU SQLite upgrade rolls back DDL marker and commit failures", "[jobu][sqlite][schema][fault]")
{
    auto const* const boundary = GENERATE("index.planned", "index.queue", "index.job", "marker", "connection");
    auto const        phase    = GENERATE(DatabaseFaultPhase::Before, DatabaseFaultPhase::AfterSuccess);
    // A committed transaction with a lost acknowledgement is tested separately: rollback cannot undo that commit.
    if (std::string_view{boundary} == "connection" && phase == DatabaseFaultPhase::AfterSuccess) {
        return;
    }
    CAPTURE(boundary, phase);
    UpgradeFixture fixture;
    auto const     operation =
        std::string_view{boundary} == "connection" ? DatabaseOperation::Commit : DatabaseOperation::Execute;
    fixture.faults->faults.push_back({
        .at    = {.boundary = boundary, .operation = operation, .phase = phase},
        .error = fault_error()
    });

    auto failed = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "jobu.schema.upgrade_failed");
    require_consumed_faults(*fixture.faults);
    fixture.require_unchanged();
    REQUIRE(std::ranges::find(fixture.faults->calls,
                              DatabaseCall{.boundary  = "connection",
                                           .operation = DatabaseOperation::Rollback,
                                           .phase = DatabaseFaultPhase::AfterSuccess}) != fixture.faults->calls.end());

    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    fixture.require_unchanged();
    auto retried = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(retried);
    CHECK(retried->upgraded);
    CHECK(storage_snapshot(fixture.database) == fixture.before);
}

TEST_CASE("JobU SQLite upgrade poisoning preserves uncertain commit semantics", "[jobu][sqlite][schema][fault]")
{
    auto const committed = GENERATE(false, true);
    CAPTURE(committed);
    UpgradeFixture fixture;
    if (committed) {
        fixture.faults->faults.push_back({
            .at    = {.boundary  = "connection",
                      .operation = DatabaseOperation::Commit,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
    }
    else {
        fixture.faults->faults.push_back({
            .at    = {.boundary  = "marker",
                      .operation = DatabaseOperation::Execute,
                      .phase     = DatabaseFaultPhase::AfterSuccess},
            .error = fault_error()
        });
        fixture.faults->faults.push_back({
            .at    = {.boundary = "connection", .operation = DatabaseOperation::Rollback},
            .error = fault_error()
        });
    }

    auto failed = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "jobu.schema.upgrade_failed");
    require_consumed_faults(*fixture.faults);
    REQUIRE(fixture.database.is_poisoned());
    REQUIRE_FALSE(jobu_sqlite::ensure_schema(fixture.database));

    // Closing the poisoned connection resolves its resources, not the already-reported startup failure.
    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    CHECK(scalar_integer(fixture.database, "SELECT version FROM jobu_schema") == (committed ? 2 : 1));
    CHECK(storage_snapshot(fixture.database) == fixture.before);
    auto reopened = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE(reopened);
    CHECK(reopened->upgraded == !committed);
}

TEST_CASE("JobU SQLite validates version one before upgrading", "[jobu][sqlite][schema]")
{
    UpgradeFixture fixture;
    execute(fixture.database, "DROP INDEX jobu_runs_job_state_idx");
    auto failed = jobu_sqlite::ensure_schema(fixture.database);
    require_schema_error(failed, ErrorCategory::Internal, "jobu.schema.invalid");
    fixture.require_unchanged();
    CHECK(std::ranges::none_of(fixture.faults->calls, [](DatabaseCall const& call) {
        return call.boundary.starts_with("index.") || call.boundary == "marker";
    }));
}

TEST_CASE("JobU SQLite rejects incompatible history indexes without repair", "[jobu][sqlite][schema]")
{
    auto const        existing_version_two = GENERATE(false, true);
    auto const* const definition           = GENERATE(
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(id DESC, planned_at_us DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us, id DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us DESC, id DESC, queue_id)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us DESC, id COLLATE NOCASE DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us DESC, id DESC) WHERE state = 'running'",
        "CREATE UNIQUE INDEX jobu_runs_planned_id_idx ON jobu_runs(planned_at_us DESC, id DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_jobs(created_at_us DESC, id DESC)",
        "CREATE INDEX jobu_runs_planned_id_idx ON jobu_runs((planned_at_us + 1) DESC, id DESC)",
        "CREATE TABLE jobu_runs_planned_id_idx (id INTEGER)");
    CAPTURE(existing_version_two, definition);
    UpgradeFixture fixture;
    if (existing_version_two) {
        REQUIRE(jobu_sqlite::ensure_schema(fixture.database));
        execute(fixture.database, "DROP INDEX jobu_runs_planned_id_idx");
    }
    execute(fixture.database, definition);
    auto failed = jobu_sqlite::ensure_schema(fixture.database);
    require_schema_error(failed,
                         ErrorCategory::Internal,
                         existing_version_two ? "jobu.schema.invalid" : "jobu.schema.upgrade_failed");
    CHECK(scalar_integer(fixture.database, "SELECT version FROM jobu_schema") == (existing_version_two ? 2 : 1));
    CHECK(storage_snapshot(fixture.database) == fixture.before);
    CHECK(scalar_integer(fixture.database,
                         "SELECT count(*) FROM sqlite_schema WHERE name = 'jobu_runs_planned_id_idx'") == 1);
}

TEST_CASE("JobU SQLite never repairs a missing version two history index", "[jobu][sqlite][schema]")
{
    auto const* const index =
        GENERATE("jobu_runs_planned_id_idx", "jobu_runs_queue_planned_id_idx", "jobu_runs_job_planned_id_idx");
    CAPTURE(index);
    UpgradeFixture fixture;
    REQUIRE(jobu_sqlite::ensure_schema(fixture.database));
    execute(fixture.database, std::string{"DROP INDEX "} + index);
    require_schema_error(jobu_sqlite::ensure_schema(fixture.database), ErrorCategory::Internal, "jobu.schema.invalid");
    CHECK(storage_snapshot(fixture.database) == fixture.before);
}

TEST_CASE("JobU SQLite upgrade observer failure leaves the original schema reopenable", "[jobu][sqlite][schema][fault]")
{
    UpgradeFixture fixture;
    auto           failed = jobu_sqlite::detail::ensure_schema_impl(fixture.database, fail_after_first_creation);
    require_schema_error(failed, ErrorCategory::Internal, "jobu.schema.upgrade_failed");
    fixture.require_unchanged();
    REQUIRE(fixture.database.close());
    REQUIRE(fixture.database.open());
    fixture.require_unchanged();
}

TEST_CASE("JobU SQLite upgrade rolls back a final manifest validation failure", "[jobu][sqlite][schema][fault]")
{
    UpgradeFixture fixture;
    // Arm the read fault only after the marker update is prepared, beyond validation of the newly created indexes.
    fixture.faults->classify = [marked = false](std::string_view sql) mutable -> std::string {
        if (sql.starts_with("UPDATE jobu_schema SET version")) {
            marked = true;
        }
        if (marked && sql.starts_with("SELECT name, desc, coll FROM pragma_index_xinfo")) {
            return "final.index";
        }
        return "other";
    };
    fixture.faults->faults.push_back({
        .at    = {.boundary = "final.index", .operation = DatabaseOperation::Fetch},
        .error = fault_error()
    });
    auto failed = jobu_sqlite::ensure_schema(fixture.database);
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "jobu.schema.upgrade_failed");
    require_consumed_faults(*fixture.faults);
    fixture.require_unchanged();
}

TEST_CASE("JobU SQLite fresh creation rolls back marker and commit failures", "[jobu][sqlite][schema][fault]")
{
    auto const         marker = GENERATE(false, true);
    TemporaryDirectory directory;
    auto               faults = std::make_shared<DatabaseFaultState>();
    faults->classify          = [](std::string_view sql) -> std::string {
        return sql.starts_with("INSERT INTO jobu_schema") ? "marker" : "other";
    };
    Database database{
        std::make_unique<FaultDatabaseDriver>(std::make_unique<jb::db::sqlite::Driver>(jb::db::sqlite::Options{
                                                  .database_file = directory.path() / "new.sqlite"}),
                                              faults)};
    REQUIRE(database.open());
    faults->faults.push_back({
        .at    = {.boundary  = marker ? "marker" : "connection",
                  .operation = marker ? DatabaseOperation::Execute : DatabaseOperation::Commit,
                  .phase     = marker ? DatabaseFaultPhase::AfterSuccess : DatabaseFaultPhase::Before},
        .error = fault_error()
    });
    auto failed = jobu_sqlite::ensure_schema(database);
    REQUIRE_FALSE(failed);
    check_safe_error(failed.error(), "jobu.schema.create_failed");
    require_consumed_faults(*faults);
    REQUIRE_FALSE(database.is_poisoned());
    CHECK(scalar_integer(database, "SELECT count(*) FROM sqlite_schema WHERE name GLOB 'jobu_*'") == 0);
    REQUIRE(database.close());
    REQUIRE(database.open());
    auto created = jobu_sqlite::ensure_schema(database);
    REQUIRE(created);
    CHECK(created->created);
    CHECK_FALSE(created->upgraded);
}

TEST_CASE("JobU SQLite upgrade rejects an ineffective marker update", "[jobu][sqlite][schema]")
{
    UpgradeFixture fixture;
    // Extra schema objects are permitted, but a trigger cannot make an unchanged marker look like an upgrade.
    execute(fixture.database,
            "CREATE TRIGGER block_version BEFORE UPDATE ON jobu_schema "
            "BEGIN SELECT RAISE(IGNORE); END");
    require_schema_error(jobu_sqlite::ensure_schema(fixture.database),
                         ErrorCategory::Internal,
                         "jobu.schema.upgrade_failed");
    fixture.require_unchanged();
}
