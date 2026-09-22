#include "secret_service.hpp"

#include "management.hpp"
#include "payload_template_priv.hpp"
#include "query.hpp"
#include "secret_provider_priv.hpp"
#include "secret_repository_priv.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

using Operation = DatabaseOperation;
using Phase     = DatabaseFaultPhase;

struct Fixture {
    std::shared_ptr<DatabaseFaultState> faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture                     storage{[this](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    FakeTimeSource                      time;
    jb::jobu::detail::SecretRepository  repository{storage.database};
    std::vector<std::string>            statements;
    std::vector<Error>                  failures;
    std::vector<DatabaseCall>           calls_at_failure;
    std::vector<DatabaseCall>           calls_at_commit;
    std::size_t                         committed{0};
    SecretService                       service{storage.database, time};

    Fixture()
    {
        time.set_utc(UtcTimePoint{10s});
        faults->classify = [this](std::string_view sql) -> std::string {
            statements.emplace_back(sql);
            if (sql.starts_with("INSERT INTO jobu_secrets")) {
                return "insert";
            }
            if (sql.starts_with("UPDATE jobu_secrets")) {
                return "update";
            }
            if (sql.starts_with("DELETE FROM jobu_secrets")) {
                return "delete";
            }
            if (sql.find("FROM jobu_secret_refs") != std::string_view::npos) {
                return "references";
            }
            if (sql.find("FROM jobu_runs") != std::string_view::npos) {
                return sql.find("AND id > :after_id") == std::string_view::npos ? "snapshots" : "later_snapshots";
            }
            return "read";
        };
        service.failed.connect(&service, [this](Error const& error) {
            failures.push_back(error);
            calls_at_failure = faults->calls;
            service.stop_mutations();
        });
        service.mutation_committed.connect(&service, [this] {
            ++committed;
            calls_at_commit = faults->calls;
        });
    }

    void seed_reference()
    {
        auto queue = recovery_queue(recovery_id(1));
        storage.insert_queue(queue);
        auto job = storage.make_job(recovery_id(2), queue.id);
        storage.insert_job(job);
        auto begun = Transaction::begin(storage.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        auto references  = std::array{
            jb::jobu::detail::SecretReference{.secret_name = "token", .field_path = "/arguments/0"}
        };
        REQUIRE(repository.replace_references_for_job(job.id, references));
        REQUIRE(transaction.commit());
    }

    void arm(DatabaseCall call, std::string code = "db.io")
    {
        faults->faults.push_back({.at = std::move(call), .error = fault_error(std::move(code))});
    }

    void check_closed_gate()
    {
        auto const calls  = faults->calls.size();
        auto       set    = service.set({});
        auto       erased = service.erase("");
        REQUIRE_FALSE(set);
        REQUIRE_FALSE(erased);
        CHECK(set.error().code == "jobu.service.stopping");
        CHECK(erased.error().code == "jobu.service.stopping");
        CHECK(faults->calls.size() == calls);
    }
};

auto payload(std::string_view text) -> JsonValue
{
    auto parsed = parse_json(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

struct ManagedFixture : Fixture {
    FakeCronEngine        cron;
    SequenceUuidGenerator generator{
        {recovery_id(1000), recovery_id(1001), recovery_id(1002), recovery_id(1003)}
    };
    ManagementService management{storage.database, storage.registry, cron, generator, time};
    Queue             queue = recovery_queue(recovery_id(1));

    ManagedFixture() { storage.insert_queue(queue); }
};

void check_error(auto const& result, ErrorCategory category, std::string_view code)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == category);
    check_safe_error(result.error(), code);
}

} // namespace

TEST_CASE("Secret service preserves binary values and upsert timestamps without exposing bytes")
{
    Fixture fixture;
    auto    first = fixture.service.set({.name = "token"});
    REQUIRE(first);
    CHECK(first->name == "token");
    CHECK(first->created_at == UtcTimePoint{10s});
    CHECK(first->updated_at == UtcTimePoint{10s});
    auto empty = fixture.repository.find_value("token");
    REQUIRE(empty);
    CHECK(empty->empty());
    REQUIRE_FALSE(fixture.calls_at_commit.empty());
    CHECK(fixture.calls_at_commit.back() ==
          DatabaseCall{.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});

    auto const bytes = ByteBuffer{std::byte{0}, std::byte{0xff}, std::byte{0x41}, std::byte{0x0a}};
    fixture.time.set_utc(UtcTimePoint{20s});
    auto rotated = fixture.service.set({.name = "token", .value = bytes});
    REQUIRE(rotated);
    CHECK(rotated->created_at == first->created_at);
    CHECK(rotated->updated_at == UtcTimePoint{20s});
    auto value = fixture.repository.find_value("token");
    REQUIRE(value);
    CHECK(*value == bytes);
    CHECK(fixture.committed == 2);

    fixture.storage.reopen();
    value = fixture.repository.find_value("token");
    REQUIRE(value);
    CHECK(*value == bytes);
    auto page = fixture.service.list({});
    REQUIRE(page);
    REQUIRE(page->items.size() == 1);
    CHECK(page->items.front().updated_at == rotated->updated_at);
    CHECK_FALSE(page->next_after_name);
    REQUIRE(fixture.service.erase("token"));
    CHECK(fixture.committed == 3);
    check_error(fixture.repository.find_value("token"), ErrorCategory::NotFound, "jobu.secret.not_found");
    check_error(fixture.service.erase("token"), ErrorCategory::NotFound, "jobu.secret.not_found");
    CHECK(fixture.committed == 3);
    CHECK(fixture.failures.empty());
}

TEST_CASE("Secret service validates names raw sizes and page boundaries before database access")
{
    Fixture fixture;
    for (auto const& name : {
             std::string{},
             std::string{"Bad.Name"},
             std::string{"bad name"},
             std::string{"a\0b", 3},
             std::string(129, 'a')
    }) {
        CAPTURE(name.size());
        auto const calls = fixture.faults->calls.size();
        check_error(fixture.service.set({.name = name}), ErrorCategory::InvalidArgument, "jobu.secret.invalid_name");
        check_error(fixture.service.erase(name), ErrorCategory::InvalidArgument, "jobu.secret.invalid_name");
        check_error(fixture.service.list({.after_name = name}),
                    ErrorCategory::InvalidArgument,
                    "jobu.secret.invalid_name");
        check_error(fixture.repository.find_value(name), ErrorCategory::InvalidArgument, "jobu.secret.invalid_name");
        CHECK(fixture.faults->calls.size() == calls);
    }
    for (auto limit : {std::size_t{0}, std::size_t{201}, std::numeric_limits<std::size_t>::max()}) {
        auto const calls = fixture.faults->calls.size();
        check_error(fixture.service.list({.limit = limit}),
                    ErrorCategory::InvalidArgument,
                    "jobu.storage.invalid_limit");
        CHECK(fixture.faults->calls.size() == calls);
    }
    auto oversized = ByteBuffer(65'537, std::byte{0xff});
    auto const calls = fixture.faults->calls.size();
    check_error(fixture.service.set({.name = "token", .value = oversized}), ErrorCategory::ResourceExhausted,
                "jobu.secret.too_large");
    check_error(fixture.repository.set("token", oversized, fixture.time.utc_now()), ErrorCategory::ResourceExhausted,
                "jobu.secret.too_large");
    CHECK(fixture.faults->calls.size() == calls);
    CHECK(fixture.committed == 0);
    CHECK(fixture.failures.empty());

    oversized.pop_back();
    REQUIRE(fixture.service.set({.name = std::string(128, 'a'), .value = oversized}));
    auto value = fixture.repository.find_value(std::string(128, 'a'));
    REQUIRE(value);
    CHECK(*value == oversized);
}

TEST_CASE("Secret pages use metadata-only lookahead even at the maximum public limit")
{
    Fixture fixture;
    auto    empty = fixture.service.list({});
    REQUIRE(empty);
    CHECK(empty->items.empty());
    CHECK_FALSE(empty->next_after_name);

    // Reverse insertion makes the asserted order independent of insertion/rowid order.
    for (int index = 200; index >= 0; --index) {
        REQUIRE(fixture.service.set({.name = "token.n" + std::to_string(1000 + index)}));
    }
    fixture.statements.clear();
    auto first = fixture.service.list({.limit = 200});
    REQUIRE(first);
    REQUIRE(first->items.size() == 200);
    CHECK(first->items.front().name == "token.n1000");
    CHECK(first->items.back().name == "token.n1199");
    CHECK(first->next_after_name == "token.n1199");
    for (auto const& sql : fixture.statements) {
        CHECK(sql.starts_with("SELECT name AS secret_name,"));
        CHECK(sql.find("value_blob") == std::string::npos);
        CHECK(sql.find('*') == std::string::npos);
    }

    // Continuations are name boundaries, not foreign keys to rows that must survive until the next call.
    REQUIRE(fixture.service.erase(*first->next_after_name));
    auto next = fixture.service.list({.limit = 200, .after_name = first->next_after_name});
    REQUIRE(next);
    REQUIRE(next->items.size() == 1);
    CHECK(next->items.front().name == "token.n1200");
    CHECK_FALSE(next->next_after_name);
    auto end = fixture.service.list({.limit = 1, .after_name = "token.n1200"});
    REQUIRE(end);
    CHECK(end->items.empty());
    CHECK_FALSE(end->next_after_name);
    auto exact = fixture.service.list({.limit = 200});
    REQUIRE(exact);
    CHECK(exact->items.size() == 200);
    CHECK_FALSE(exact->next_after_name);
    auto defaults = fixture.service.list({});
    REQUIRE(defaults);
    CHECK(defaults->items.size() == 100);
    CHECK(defaults->next_after_name == "token.n1099");
}

TEST_CASE("Secret deletion checks current references inside its transaction and remains nonfatal")
{
    Fixture fixture;
    REQUIRE(fixture.service.set({.name = "token"}));
    fixture.seed_reference();
    fixture.faults->calls.clear();
    check_error(fixture.service.erase("token"), ErrorCategory::Conflict, "jobu.secret.in_use");
    REQUIRE_FALSE(fixture.faults->calls.empty());
    CHECK(fixture.faults->calls.front() == DatabaseCall{.boundary = "connection", .operation = Operation::Begin});
    CHECK(fixture.faults->calls.back() ==
          DatabaseCall{.boundary = "connection", .operation = Operation::Rollback, .phase = Phase::AfterSuccess});
    CHECK(std::ranges::none_of(fixture.faults->calls, [](auto const& call) { return call.boundary == "delete"; }));
    CHECK(fixture.failures.empty());
    CHECK(fixture.committed == 1);
    REQUIRE(fixture.service.set({.name = "token", .value = {std::byte{0x42}}}));
    REQUIRE(fixture.repository.find_value("token"));
}

TEST_CASE("Secret construction and stop gates do no database work and stopped reads remain available")
{
    Fixture    fixture;
    auto const calls = fixture.faults->calls.size();
    {
        SecretService unused{fixture.storage.database, fixture.time};
        unused.stop_mutations();
    }
    CHECK(fixture.faults->calls.size() == calls);
    REQUIRE(fixture.service.set({.name = "token"}));
    fixture.service.stop_mutations();
    fixture.service.stop_mutations();
    fixture.check_closed_gate();
    auto page = fixture.service.list({});
    REQUIRE(page);
    CHECK(page->items.size() == 1);
    CHECK(fixture.failures.empty());
    CHECK(fixture.committed == 1);
}

TEST_CASE("Secret mutation storage faults roll back before failure notification and close admission")
{
    for (auto const* mutation : {"insert", "update", "delete"}) {
        auto checkpoints = std::vector<DatabaseCall>{
            {.boundary = "connection", .operation = Operation::Begin},
            {.boundary = "read", .operation = Operation::Fetch},
            {.boundary = mutation, .operation = Operation::Prepare},
            {.boundary = mutation, .operation = Operation::Bind},
            {.boundary = mutation, .operation = Operation::Execute},
            {.boundary = mutation, .operation = Operation::Execute, .phase = Phase::AfterSuccess},
            {.boundary = "connection", .operation = Operation::Commit}
        };
        if (std::string_view{mutation} == "delete") {
            checkpoints[1].boundary = "references";
        }
        for (auto const& checkpoint : checkpoints) {
            DYNAMIC_SECTION(mutation << " " << checkpoint.boundary << " " << static_cast<int>(checkpoint.operation)
                                     << " " << static_cast<int>(checkpoint.phase))
            {
                Fixture fixture;
                if (std::string_view{mutation} != "insert") {
                    REQUIRE(fixture.service.set({.name = "token", .value = {std::byte{0x42}}}));
                }
                fixture.committed = 0;
                auto const before = storage_snapshot(fixture.storage.database);
                fixture.arm(checkpoint);
                if (std::string_view{mutation} == "delete") {
                    check_error(fixture.service.erase("token"), ErrorCategory::Io, "db.io");
                }
                else {
                    check_error(fixture.service.set({.name = "token", .value = {std::byte{0xff}}}),
                                ErrorCategory::Io,
                                "db.io");
                }
                require_consumed_faults(*fixture.faults);
                REQUIRE(fixture.failures.size() == 1);
                check_safe_error(fixture.failures.front(), "db.io");
                CHECK(fixture.committed == 0);
                if (checkpoint.operation != Operation::Begin) {
                    CHECK(fixture.calls_at_failure.back() == DatabaseCall{.boundary  = "connection",
                                                                          .operation = Operation::Rollback,
                                                                          .phase     = Phase::AfterSuccess});
                }
                fixture.check_closed_gate();
                CHECK_FALSE(fixture.storage.database.is_poisoned());
                CHECK(storage_snapshot(fixture.storage.database) == before);
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);
            }
        }
    }
}

TEST_CASE("Secret commit acknowledgement failure retains durable state but never emits success")
{
    Fixture fixture;
    fixture.arm({.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
    check_error(fixture.service.set({.name = "token", .value = {std::byte{0xff}}}), ErrorCategory::Io, "db.io");
    require_consumed_faults(*fixture.faults);
    REQUIRE(fixture.failures.size() == 1);
    check_safe_error(fixture.failures.front(), "db.io");
    CHECK(fixture.committed == 0);
    fixture.check_closed_gate();
    fixture.storage.reopen();
    auto value = fixture.repository.find_value("token");
    REQUIRE(value);
    CHECK(*value == ByteBuffer{std::byte{0xff}});
}

TEST_CASE("Secret rollback poisoning promotes ordinary errors and preserves an existing fatal cause")
{
    for (auto const* scenario : {"missing", "in_use", "fatal"}) {
        DYNAMIC_SECTION(scenario)
        {
            Fixture fixture;
            if (std::string_view{scenario} == "in_use") {
                REQUIRE(fixture.service.set({.name = "token"}));
                fixture.seed_reference();
            }
            fixture.committed = 0;
            auto const before = storage_snapshot(fixture.storage.database);
            fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
            if (std::string_view{scenario} == "fatal") {
                fixture.arm({.boundary = "insert", .operation = Operation::Execute}, "db.corrupt");
                check_error(fixture.service.set({.name = "token"}), ErrorCategory::Io, "db.corrupt");
            }
            else {
                check_error(fixture.service.erase("token"), ErrorCategory::Io, "db.rollback_failed");
            }
            require_consumed_faults(*fixture.faults);
            REQUIRE(fixture.failures.size() == 1);
            CHECK(fixture.committed == 0);
            CHECK(fixture.storage.database.is_poisoned());
            CHECK(fixture.calls_at_failure.back() ==
                  DatabaseCall{.boundary = "connection", .operation = Operation::Rollback});
            auto const first_failure = fixture.failures.front();
            fixture.check_closed_gate();
            REQUIRE_FALSE(fixture.service.list({}));
            CHECK(fixture.failures.size() == 1);
            CHECK(fixture.failures.front().code == first_failure.code);
            fixture.storage.reopen();
            CHECK(storage_snapshot(fixture.storage.database) == before);
        }
    }
}

TEST_CASE("Secret metadata read failures distinguish transient I/O from corruption")
{
    for (auto const* code : {"db.io", "db.corrupt", "db.constraint"}) {
        DYNAMIC_SECTION(code)
        {
            Fixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            fixture.committed = 0;
            fixture.arm({.boundary = "read", .operation = Operation::Fetch}, code);
            check_error(fixture.service.list({}), ErrorCategory::Io, code);
            require_consumed_faults(*fixture.faults);
            CHECK(fixture.committed == 0);
            if (std::string_view{code} == "db.io") {
                CHECK(fixture.failures.empty());
                REQUIRE(fixture.service.set({.name = "other"}));
            }
            else {
                REQUIRE(fixture.failures.size() == 1);
                fixture.check_closed_gate();
            }
            // Read admission remains available even after a non-poisoning fatal failure.
            REQUIRE(fixture.service.list({}));
        }
    }
}

TEST_CASE("Secret durable metadata and raw values reject malformed storage")
{
    SECTION("Malformed metadata fails closed with fixed diagnostics")
    {
        Fixture fixture;
        REQUIRE(fixture.service.set({.name = "token"}));
        {
            Query query{fixture.storage.database};
            REQUIRE(query.exec("UPDATE jobu_secrets SET name = 'Bad.Name'"));
        }
        check_error(fixture.service.list({}), ErrorCategory::Internal, "jobu.storage.invariant");
        REQUIRE(fixture.failures.size() == 1);
        fixture.check_closed_gate();
    }
    for (auto const* value : {"'private-backend-marker'", "zeroblob(65537)"}) {
        DYNAMIC_SECTION(value)
        {
            Fixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            {
                Query query{fixture.storage.database};
                // Bypass only the schema CHECK to exercise durable decode of externally damaged bytes.
                REQUIRE(query.exec("PRAGMA ignore_check_constraints = ON"));
                REQUIRE(query.exec(std::string{"UPDATE jobu_secrets SET value_blob = "} + value));
                REQUIRE(query.exec("PRAGMA ignore_check_constraints = OFF"));
            }
            auto read = fixture.repository.find_value("token");
            REQUIRE_FALSE(read);
            CHECK(read.error().code.starts_with("jobu.storage."));
            CHECK(read.error().message.find("private-backend-marker") == std::string::npos);
            CHECK(read.error().detail.find("private-backend-marker") == std::string::npos);
            // Administrative metadata never decodes/selects the damaged value column.
            REQUIRE(fixture.service.list({}));
        }
    }
}

TEST_CASE("Private secret lookup obeys caller transaction and propagates represented read failures")
{
    Fixture fixture;
    {
        auto begun = Transaction::begin(fixture.storage.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        REQUIRE(fixture.repository.set("token", {}, fixture.time.utc_now()));
        REQUIRE(fixture.repository.find_value("token"));
        // Lookup must not commit the caller's uncommitted secret on return.
    }
    check_error(fixture.repository.find_value("token"), ErrorCategory::NotFound, "jobu.secret.not_found");
    for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch}) {
        fixture.arm({.boundary = "read", .operation = operation});
        auto value = fixture.repository.find_value("token");
        REQUIRE_FALSE(value);
        CHECK(value.error().code == "db.io");
    }
    require_consumed_faults(*fixture.faults);
}

TEST_CASE("Management maintains recognized references and replay ignores later secret rotation or deletion")
{
    ManagedFixture fixture;
    REQUIRE(fixture.service.set({.name = "token"}));
    auto original = payload(
        R"({"command":"/bin/tool","arguments":[{"secret":"token"},{"secret":"token"}],"future":{"secret":"ignored"}})");
    auto request = CreateJobRequest{.queue           = fixture.queue.id,
                                    .schedule        = OnceSchedule{UtcTimePoint{20s}},
                                    .payload         = original,
                                    .idempotency_key = "create"};
    auto created = fixture.management.create_job(request);
    REQUIRE(created);
    CHECK(*fixture.repository.reference_count("token") == 2);
    check_error(fixture.service.erase("token"), ErrorCategory::Conflict, "jobu.secret.in_use");

    REQUIRE(fixture.service.set({.name = "token", .value = {std::byte{0xff}}}));
    fixture.statements.clear();
    auto replay = fixture.management.create_job(request);
    REQUIRE(replay);
    CHECK(replay->payload == original);
    CHECK(replay->id == created->id);
    for (auto const& sql : fixture.statements) {
        CHECK(sql.find("jobu_secrets") == std::string::npos);
        CHECK(sql.find("jobu_secret_refs") == std::string::npos);
    }

    REQUIRE(fixture.service.set({.name = "body"}));
    auto replacement = payload(R"({"url":"https://example.test/","method":"POST","body":{"secret":"body"}})");
    auto before      = storage_snapshot(fixture.storage.database);
    check_error(fixture.management.update_job(
                    {.job_id = created->id, .expected_revision = 2, .type = JobType::Http, .payload = replacement}),
                ErrorCategory::Conflict,
                "jobu.job.revision_conflict");
    CHECK(storage_snapshot(fixture.storage.database) == before);
    auto updated = fixture.management.update_job(
        {.job_id = created->id, .expected_revision = 1, .type = JobType::Http, .payload = replacement});
    REQUIRE(updated);
    CHECK(*fixture.repository.reference_count("token") == 0);
    CHECK(*fixture.repository.reference_count("body") == 1);
    REQUIRE(fixture.service.erase("token"));

    // The pending schedule-owned snapshot was refreshed, so the old secret can be removed. Replay must neither
    // check that now-missing name nor resurrect its reference rows; the recorded revision remains unchanged.
    fixture.statements.clear();
    replay = fixture.management.create_job(request);
    REQUIRE(replay);
    CHECK(replay->id == created->id);
    CHECK(replay->revision == 1);
    CHECK(replay->payload == original);
    for (auto const& sql : fixture.statements) {
        CHECK(sql.find("jobu_secrets") == std::string::npos);
        CHECK(sql.find("jobu_secret_refs") == std::string::npos);
    }
    CHECK(*fixture.repository.reference_count("token") == 0);
    CHECK(*fixture.repository.reference_count("body") == 1);

    // An update retaining the payload must retain its references too; changing type alone cannot invalidate it.
    updated = fixture.management.update_job({.job_id = created->id, .expected_revision = 2, .priority = 8});
    REQUIRE(updated);
    CHECK(*fixture.repository.reference_count("body") == 1);
    before = storage_snapshot(fixture.storage.database);
    check_error(fixture.management.update_job({.job_id = created->id, .expected_revision = 3, .type = JobType::Cli}),
                ErrorCategory::InvalidArgument,
                "jobu.job.invalid_payload");
    CHECK(storage_snapshot(fixture.storage.database) == before);
}

TEST_CASE("Older executable snapshots protect secrets after recurring definition replacement")
{
    for (auto state : {RunState::Scheduled, RunState::Running, RunState::RetryWait}) {
        DYNAMIC_SECTION(static_cast<int>(state))
        {
            ManagedFixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            auto job     = fixture.storage.make_job(recovery_id(2), fixture.queue.id);
            job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
            fixture.cron.set_occurrences(std::get<CronSchedule>(job.schedule), {UtcTimePoint{20s}});
            job.payload = payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
            fixture.storage.insert_job(job);
            auto refs = jb::jobu::detail::validate_payload_template(job.type, job.payload);
            REQUIRE(refs);
            REQUIRE(fixture.repository.replace_references_for_job(job.id, *refs));
            auto run = fixture.storage.make_run(recovery_id(3), job, state, state == RunState::RetryWait ? 1 : 0);
            fixture.storage.insert_run(run);
            // A manual pending run keeps its original snapshot even when the schedule-owned run is refreshed.
            auto manual = fixture.storage.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual);
            fixture.storage.insert_run(manual);
            REQUIRE(fixture.management.update_job(
                {.job_id = job.id, .expected_revision = 1, .payload = payload(R"({"command":"/bin/new"})")}));
            CHECK(*fixture.repository.reference_count("token") == 0);
            check_error(fixture.service.erase("token"), ErrorCategory::Conflict, "jobu.secret.in_use");
            CHECK(fixture.failures.empty());
            fixture.storage.reopen();
            check_error(fixture.service.erase("token"), ErrorCategory::Conflict, "jobu.secret.in_use");
        }
    }
}

TEST_CASE("Snapshot protection includes every nonterminal state and ignores terminal history")
{
    for (auto state : {RunState::Scheduled,
                       RunState::Running,
                       RunState::RetryWait,
                       RunState::Succeeded,
                       RunState::Failed,
                       RunState::Interrupted,
                       RunState::Cancelled}) {
        for (auto origin : {RunOrigin::Scheduled, RunOrigin::Manual}) {
            DYNAMIC_SECTION(static_cast<int>(state) << " origin=" << static_cast<int>(origin))
            {
                Fixture fixture;
                REQUIRE(fixture.service.set({.name = "token"}));
                // Even tombstoned owners must not hide a surviving executable snapshot from this protection.
                auto queue = recovery_queue(recovery_id(1), QueueState::Deleted);
                fixture.storage.insert_queue(queue);
                auto job       = fixture.storage.make_job(recovery_id(2), queue.id, JobType::Http);
                job.state      = JobState::Deleted;
                job.deleted_at = UtcTimePoint{100s};
                fixture.storage.insert_job(job);
                auto run =
                    fixture.storage.make_run(recovery_id(3), job, state, state == RunState::RetryWait ? 1 : 0, origin);
                run.run.payload =
                    payload(R"({"url":"https://example.test/","method":"POST","body":{"secret":"token"}})");
                fixture.storage.insert_run(run);
                auto erased = fixture.service.erase("token");
                if (state == RunState::Scheduled || state == RunState::Running || state == RunState::RetryWait) {
                    check_error(erased, ErrorCategory::Conflict, "jobu.secret.in_use");
                }
                else {
                    REQUIRE(erased);
                }
                CHECK(fixture.failures.empty());
            }
        }
    }
}

TEST_CASE("Job and queue deletion remove current references and cancel waiting snapshots atomically")
{
    for (bool delete_queue : {false, true}) {
        DYNAMIC_SECTION("queue=" << delete_queue)
        {
            ManagedFixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            auto created = fixture.management.create_job(
                {.queue    = fixture.queue.id,
                 .schedule = OnceSchedule{UtcTimePoint{20s}},
                 .payload  = payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})")});
            REQUIRE(created);
            REQUIRE(fixture.management.run_now({.job_id = created->id}));
            REQUIRE(fixture.management.suspend_job(created->id));
            CHECK(*fixture.repository.reference_count("token") == 1);
            REQUIRE(fixture.management.resume_job(created->id));
            CHECK(*fixture.repository.reference_count("token") == 1);
            auto suspended = fixture.management.suspend_job(created->id);
            REQUIRE(suspended);
            auto other = recovery_queue(recovery_id(5));
            other.name = "other";
            fixture.storage.insert_queue(other);
            auto moved = fixture.management.move_job(
                {.job_id = created->id, .expected_revision = suspended->revision, .target_queue = other.id});
            REQUIRE(moved);
            CHECK(*fixture.repository.reference_count("token") == 1);
            if (delete_queue) {
                REQUIRE(fixture.management.suspend_queue(other.id));
                REQUIRE(fixture.management.delete_queue(other.id));
            }
            else {
                REQUIRE(fixture.management.delete_job({.job_id = created->id, .expected_revision = moved->revision}));
            }
            CHECK(*fixture.repository.reference_count("token") == 0);
            REQUIRE(fixture.service.erase("token"));
        }
    }
}

TEST_CASE("Secret deletion exhausts bounded snapshot pages and finds boundary references")
{
    for (unsigned match : {0U, 100U, 101U, 205U}) {
        DYNAMIC_SECTION("match=" << match)
        {
            Fixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            auto queue = recovery_queue(recovery_id(1));
            fixture.storage.insert_queue(queue);
            auto job = fixture.storage.make_job(recovery_id(2), queue.id);
            fixture.storage.insert_job(job);
            // Reverse insertion and manual runs exercise UUID keyset order without the schedule-owned uniqueness key.
            for (unsigned i = 205; i > 0; --i) {
                auto run =
                    fixture.storage.make_run(recovery_id(1000 + i), job, RunState::Scheduled, 0, RunOrigin::Manual);
                if (i == match) {
                    run.run.payload = payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
                }
                fixture.storage.insert_run(run);
            }
            fixture.statements.clear();
            fixture.faults->calls.clear();
            auto erased = fixture.service.erase("token");
            if (match == 0) {
                REQUIRE(erased);
            }
            else {
                check_error(erased, ErrorCategory::Conflict, "jobu.secret.in_use");
            }
            auto pages = std::size_t{0};
            for (auto const& sql : fixture.statements) {
                CHECK(sql.find("value_blob") == std::string::npos);
                if (sql.find("FROM jobu_runs") != std::string::npos) {
                    ++pages;
                    CHECK(sql.find("LIMIT :limit") != std::string::npos);
                    CHECK(sql.find("result") == std::string::npos);
                    CHECK(sql.find("output") == std::string::npos);
                    CHECK(sql.find("JOIN") == std::string::npos);
                }
            }
            CHECK(pages == (match == 0 ? 3 : (match + 99) / 100));
            CHECK(fixture.faults->calls.front() ==
                  DatabaseCall{.boundary = "connection", .operation = Operation::Begin});
            CHECK(fixture.failures.empty());
        }
    }
}

TEST_CASE("Malformed live templates fail secret deletion closed without exposing persisted content")
{
    for (auto const* document :
         {"{", "{}", R"({"command":"/bin/tool","arguments":[{"secret":"private-backend-marker","extra":1}]})"}) {
        DYNAMIC_SECTION(document)
        {
            Fixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            auto queue = recovery_queue(recovery_id(1));
            fixture.storage.insert_queue(queue);
            auto job = fixture.storage.make_job(recovery_id(2), queue.id);
            fixture.storage.insert_job(job);
            fixture.storage.insert_run(fixture.storage.make_run(recovery_id(3), job));
            {
                Query query{fixture.storage.database};
                REQUIRE(query.prepare("UPDATE jobu_runs SET payload_json = :payload"));
                REQUIRE(query.bind_value(":payload", make_text(document)));
                REQUIRE(query.exec());
            }
            auto before       = storage_snapshot(fixture.storage.database);
            fixture.committed = 0;
            auto result       = fixture.service.erase("token");
            REQUIRE_FALSE(result);
            CHECK(result.error().code.starts_with("jobu.storage."));
            check_safe_error(result.error(), result.error().code);
            REQUIRE(fixture.failures.size() == 1);
            CHECK(fixture.calls_at_failure.back() == DatabaseCall{.boundary  = "connection",
                                                                  .operation = Operation::Rollback,
                                                                  .phase     = Phase::AfterSuccess});
            fixture.check_closed_gate();
            CHECK(fixture.committed == 0);
            CHECK(storage_snapshot(fixture.storage.database) == before);
        }
    }
}

TEST_CASE("Snapshot scan faults and rollback poisoning preserve deletion protection")
{
    for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch}) {
        DYNAMIC_SECTION(static_cast<int>(operation))
        {
            Fixture fixture;
            REQUIRE(fixture.service.set({.name = "token"}));
            auto before       = storage_snapshot(fixture.storage.database);
            fixture.committed = 0;
            fixture.arm({.boundary = "snapshots", .operation = operation});
            check_error(fixture.service.erase("token"), ErrorCategory::Io, "db.io");
            require_consumed_faults(*fixture.faults);
            REQUIRE(fixture.failures.size() == 1);
            CHECK(fixture.calls_at_failure.back() == DatabaseCall{.boundary  = "connection",
                                                                  .operation = Operation::Rollback,
                                                                  .phase     = Phase::AfterSuccess});
            fixture.check_closed_gate();
            CHECK(fixture.committed == 0);
            CHECK(storage_snapshot(fixture.storage.database) == before);
        }
    }
    SECTION("An ordinary snapshot conflict becomes fatal if rollback fails")
    {
        Fixture fixture;
        REQUIRE(fixture.service.set({.name = "token"}));
        auto queue = recovery_queue(recovery_id(1));
        fixture.storage.insert_queue(queue);
        auto job = fixture.storage.make_job(recovery_id(2), queue.id);
        fixture.storage.insert_job(job);
        auto run        = fixture.storage.make_run(recovery_id(3), job);
        run.run.payload = payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
        fixture.storage.insert_run(run);
        auto before       = storage_snapshot(fixture.storage.database);
        fixture.committed = 0;
        fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
        check_error(fixture.service.erase("token"), ErrorCategory::Io, "db.rollback_failed");
        require_consumed_faults(*fixture.faults);
        REQUIRE(fixture.failures.size() == 1);
        CHECK(fixture.storage.database.is_poisoned());
        fixture.check_closed_gate();
        CHECK(fixture.committed == 0);
        fixture.storage.reopen();
        CHECK(storage_snapshot(fixture.storage.database) == before);
    }
}

TEST_CASE("A later snapshot page failure cannot be mistaken for a completed deletion scan")
{
    for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch}) {
        for (auto phase : {Phase::Before, Phase::AfterSuccess}) {
            DYNAMIC_SECTION(static_cast<int>(operation) << " phase=" << static_cast<int>(phase))
            {
                Fixture fixture;
                REQUIRE(fixture.service.set({.name = "token"}));
                auto queue = recovery_queue(recovery_id(1));
                fixture.storage.insert_queue(queue);
                auto job = fixture.storage.make_job(recovery_id(2), queue.id);
                fixture.storage.insert_job(job);
                for (unsigned i = 0; i < 101; ++i) {
                    auto run =
                        fixture.storage.make_run(recovery_id(1000 + i), job, RunState::Scheduled, 0, RunOrigin::Manual);
                    fixture.storage.insert_run(run);
                }
                auto before       = storage_snapshot(fixture.storage.database);
                fixture.committed = 0;
                fixture.arm({.boundary = "later_snapshots", .operation = operation, .phase = phase});
                check_error(fixture.service.erase("token"), ErrorCategory::Io, "db.io");
                require_consumed_faults(*fixture.faults);
                REQUIRE(fixture.failures.size() == 1);
                CHECK(fixture.calls_at_failure.back() == DatabaseCall{.boundary  = "connection",
                                                                      .operation = Operation::Rollback,
                                                                      .phase     = Phase::AfterSuccess});
                fixture.check_closed_gate();
                CHECK(fixture.committed == 0);
                CHECK(storage_snapshot(fixture.storage.database) == before);
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);
            }
        }
    }
}

TEST_CASE("Definition existence checks use metadata and never inspect private secret bytes")
{
    ManagedFixture fixture;
    REQUIRE(fixture.service.set({.name = "token", .value = {std::byte{0xff}}}));
    fixture.statements.clear();
    auto created = fixture.management.create_job(
        {.queue    = fixture.queue.id,
         .schedule = OnceSchedule{UtcTimePoint{20s}},
         .payload  = payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"},{"secret":"token"}]})")});
    REQUIRE(created);
    auto metadata_reads = std::size_t{0};
    for (auto const& sql : fixture.statements) {
        CHECK(sql.find("value_blob") == std::string::npos);
        if (sql.find("FROM jobu_secrets") != std::string::npos) {
            ++metadata_reads;
            CHECK(sql.starts_with("SELECT name AS secret_name,"));
        }
    }
    CHECK(metadata_reads == 1);
    CHECK(created->payload ==
          payload(R"({"command":"/bin/tool","arguments":[{"secret":"token"},{"secret":"token"}]})"));
}

TEST_CASE("Database secret provider borrows transactions and returns owning binary values")
{
    Fixture fixture;
    fixture.faults->calls.clear();
    jb::jobu::detail::DatabaseSecretProvider provider{fixture.storage.database};
    CHECK(fixture.faults->calls.empty());
    check_error(provider.resolve("token"), ErrorCategory::NotFound, "jobu.secret.not_found");

    auto const value = ByteBuffer{std::byte{0}, std::byte{0xff}, std::byte{7}};
    {
        auto begun = Transaction::begin(fixture.storage.database);
        REQUIRE(begun);
        auto transaction = std::move(begun).value();
        REQUIRE(fixture.repository.set("token", value, fixture.time.utc_now()));
        fixture.faults->calls.clear();
        auto resolved = provider.resolve("token");
        REQUIRE(resolved);
        CHECK(*resolved == value);
        for (auto const& call : fixture.faults->calls) {
            CHECK(call.operation != Operation::Begin);
            CHECK(call.operation != Operation::Commit);
            CHECK(call.operation != Operation::Rollback);
        }
        REQUIRE(fixture.repository.set("token", {}, fixture.time.utc_now()));
        auto empty = provider.resolve("token");
        REQUIRE(empty);
        CHECK(empty->empty());
        CHECK(*resolved == value);
        // Scope rollback proves neither lookup committed the caller's writes.
    }
    check_error(provider.resolve("token"), ErrorCategory::NotFound, "jobu.secret.not_found");
}

TEST_CASE("Database preparation preserves durable data and propagates lookup failure provenance")
{
    using namespace jb::jobu::detail;
    Fixture                fixture;
    DatabaseSecretProvider provider{fixture.storage.database};
    REQUIRE(fixture.service.set({.name = "token", .value = {std::byte{0xff}}}));
    auto       original = payload(R"({"url":"https://example.test/","method":"POST","body":{"secret":"token"}})");
    auto const before   = storage_snapshot(fixture.storage.database);
    REQUIRE(prepare_payload_template(JobType::Http, original, provider));
    CHECK(storage_snapshot(fixture.storage.database) == before);

    for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch}) {
        fixture.arm({.boundary = "read", .operation = operation});
        auto prepared = prepare_payload_template(JobType::Http, original, provider);
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().kind == PayloadPreparationFailureKind::Storage);
        check_safe_error(prepared.error().error, "db.io");
        CHECK(storage_snapshot(fixture.storage.database) == before);
    }
    require_consumed_faults(*fixture.faults);

    for (auto const* value : {"'private-backend-marker'", "zeroblob(65537)"}) {
        {
            Query query{fixture.storage.database};
            // Simulate externally corrupted storage, bypassing only its value constraint.
            REQUIRE(query.exec("PRAGMA ignore_check_constraints = ON"));
            REQUIRE(query.exec(std::string{"UPDATE jobu_secrets SET value_blob = "} + value));
            REQUIRE(query.exec("PRAGMA ignore_check_constraints = OFF"));
        }
        auto prepared = prepare_payload_template(JobType::Http, original, provider);
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().kind == PayloadPreparationFailureKind::PersistedData);
        CHECK(prepared.error().error.message.find("private-backend-marker") == std::string::npos);
        CHECK(prepared.error().error.detail.find("private-backend-marker") == std::string::npos);
    }
}
