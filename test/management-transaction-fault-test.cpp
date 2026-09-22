#include "management.hpp"

#include "secret_repository_priv.hpp"

#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/storage_fault_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

// Classify by the operation's table, so multi-table mutations get independent write boundaries.
auto boundary(std::string_view sql) -> std::string
{
    for (auto const& [table, name] : std::array<std::pair<std::string_view, std::string_view>, 5>{
             {{"jobu_queues", "queue"},
              {"jobu_jobs", "job"},
              {"jobu_runs", "run"},
              {"jobu_idempotency", "idempotency"},
              {"jobu_secret_refs", "references"}}
    }) {
        for (auto const& [prefix, action] : std::array<std::pair<std::string_view, std::string_view>, 3>{
                 {{"INSERT INTO ", ".insert"}, {"UPDATE ", ".update"}, {"DELETE FROM ", ".delete"}}
        }) {
            if (sql.starts_with(std::string{prefix} + std::string{table})) {
                return std::string{name} + std::string{action};
            }
        }
    }
    if (sql.find("FROM jobu_secrets") != std::string_view::npos) {
        return "secret.read";
    }
    return "read";
}

struct Fixture {
    std::shared_ptr<DatabaseFaultState> faults = std::make_shared<DatabaseFaultState>();
    RecoveryFixture                     storage{[this](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    FakeCronEngine                      cron;
    FakeTimeSource                      time;
    SequenceUuidGenerator               generator{
        {recovery_id(100), recovery_id(101), recovery_id(102)}
    };
    std::vector<Error>        failures;
    std::vector<DatabaseCall> calls_at_failure;
    std::size_t               committed{0};
    ManagementService         service{storage.database, storage.registry, cron, generator, time};

    Fixture()
    {
        time.set_utc(UtcTimePoint{120s});
        faults->classify = boundary;
        service.failed.connect(&service, [this](Error const& error) {
            failures.push_back(error);
            calls_at_failure = faults->calls;
        });
        service.mutation_committed.connect(&service, [this] { ++committed; });
    }

    void arm(DatabaseCall call, std::string code = "db.io")
    {
        faults->faults.push_back({.at = std::move(call), .error = fault_error(std::move(code))});
    }

    void require_faults_fired() const { require_consumed_faults(*faults); }

    void check_terminal_failure(std::string_view code, bool failed_rollback = true) const
    {
        CHECK(failures.size() == 1U);
        if (!failures.empty()) {
            check_safe_error(failures.front(), code);
            REQUIRE_FALSE(calls_at_failure.empty());
            // Even failed rollback must finish unwinding before the service notifies the runtime.
            CHECK(calls_at_failure.back() ==
                  DatabaseCall{.boundary  = "connection",
                               .operation = Operation::Rollback,
                               .phase     = failed_rollback ? Phase::Before : Phase::AfterSuccess});
        }
        CHECK(committed == 0U);
    }

    void check_closed_gate()
    {
        auto const calls = faults->calls.size();
        auto       check = [](auto const& result) {
            REQUIRE_FALSE(result);
            CHECK(result.error().code == "jobu.service.stopping");
        };
        // Gate every public mutation before validation, including C++-only Run Now.
        check(service.create_queue({}));
        check(service.update_queue({}));
        check(service.suspend_queue(std::string{}));
        check(service.resume_queue(std::string{}));
        check(service.delete_queue(std::string{}));
        check(service.create_job({}));
        check(service.update_job({}));
        check(service.suspend_job(recovery_id(2)));
        check(service.resume_job(recovery_id(2)));
        check(service.move_job({}));
        check(service.delete_job({}));
        check(service.run_now({.job_id = recovery_id(2), .idempotency_key = ""}));
        CHECK(faults->calls.size() == calls);
        CHECK(failures.size() == 1U);
        CHECK(committed == 0U);
    }

    void check_poisoned_connection()
    {
        auto const calls  = faults->calls.size();
        auto       reused = storage.database.transaction();
        REQUIRE_FALSE(reused);
        CHECK(reused.error().code == "db.connection_failed");
        CHECK(faults->calls.size() == calls);
    }
};

enum class Mutation : std::uint8_t {
    CreateQueue,
    UpdateQueue,
    SuspendQueue,
    ResumeQueue,
    DeleteQueue,
    CreateJob,
    CreateImmediateJob,
    UpdateJob,
    SuspendJob,
    ResumeJob,
    MoveJob,
    DeleteJob,
    RunNow
};

constexpr auto mutations = {Mutation::CreateQueue,
                            Mutation::UpdateQueue,
                            Mutation::SuspendQueue,
                            Mutation::ResumeQueue,
                            Mutation::DeleteQueue,
                            Mutation::CreateJob,
                            Mutation::CreateImmediateJob,
                            Mutation::UpdateJob,
                            Mutation::SuspendJob,
                            Mutation::ResumeJob,
                            Mutation::MoveJob,
                            Mutation::DeleteJob,
                            Mutation::RunNow};

auto discard_value(auto result) -> Result<void, Error>
{
    return result ? Result<void, Error>::success() : Result<void, Error>::failure(std::move(result).error());
}

struct MutationFixture : Fixture {
    Queue         queue = recovery_queue(recovery_id(1));
    JobDefinition job;

    explicit MutationFixture(Mutation mutation)
    {
        // Each request starts from a valid, changing state; no matrix case can stop at a no-op or conflict.
        if (mutation == Mutation::ResumeQueue || mutation == Mutation::DeleteQueue) {
            queue.state = QueueState::Suspended;
        }
        storage.insert_queue(queue);
        storage.insert_queue(recovery_queue(recovery_id(4)));
        job          = storage.make_job(recovery_id(2), queue.id);
        job.schedule = OnceSchedule{UtcTimePoint{180s}};
        if (mutation == Mutation::ResumeJob || mutation == Mutation::MoveJob || mutation == Mutation::DeleteJob) {
            job.state = JobState::Suspended;
        }
        auto payload =
            parse_json(R"({"command":"/bin/tool","arguments":[{"secret":"old.token"},{"secret":"old.token"}]})");
        REQUIRE(payload);
        job.payload = *payload;
        jb::jobu::detail::SecretRepository secrets{storage.database};
        REQUIRE(secrets.set("old.token", {}, time.utc_now()));
        REQUIRE(secrets.set("new.token", {}, time.utc_now()));
        storage.insert_job(job);
        auto references = jb::jobu::detail::validate_payload_template(job.type, job.payload);
        REQUIRE(references);
        REQUIRE(secrets.replace_references_for_job(job.id, *references));
        auto run            = storage.make_run(recovery_id(3), job);
        run.run.planned_at  = UtcTimePoint{180s};
        run.run.runnable_at = UtcTimePoint{180s};
        storage.insert_run(run);
        faults->calls.clear();
    }

    auto mutate(Mutation mutation) -> Result<void, Error>
    {
        switch (mutation) {
            case Mutation::CreateQueue:
                return discard_value(service.create_queue({.name = "new-queue", .idempotency_key = "create"}));
            case Mutation::UpdateQueue:
                return discard_value(service.update_queue({.queue = queue.id, .weight = 2}));
            case Mutation::SuspendQueue:
                return discard_value(service.suspend_queue(queue.id));
            case Mutation::ResumeQueue:
                return discard_value(service.resume_queue(queue.id));
            case Mutation::DeleteQueue:
                return service.delete_queue(queue.id);
            case Mutation::CreateJob:
            case Mutation::CreateImmediateJob: {
                auto schedule = JobCreationSchedule{std::get<OnceSchedule>(job.schedule)};
                if (mutation == Mutation::CreateImmediateJob) {
                    schedule = ImmediateSchedule{};
                }
                return discard_value(service.create_job({.queue           = queue.id,
                                                         .schedule        = std::move(schedule),
                                                         .payload         = job.payload,
                                                         .idempotency_key = "create"}));
            }
            case Mutation::UpdateJob: {
                auto payload = parse_json(
                    R"({"command":"/bin/tool","arguments":[{"secret":"new.token"},{"secret":"new.token"}]})");
                REQUIRE(payload);
                return discard_value(service.update_job(
                    {.job_id = job.id, .expected_revision = job.revision, .priority = 9, .payload = *payload}));
            }
            case Mutation::SuspendJob:
                return discard_value(service.suspend_job(job.id));
            case Mutation::ResumeJob:
                return discard_value(service.resume_job(job.id));
            case Mutation::MoveJob:
                return discard_value(service.move_job(
                    {.job_id = job.id, .expected_revision = job.revision, .target_queue = recovery_id(4)}));
            case Mutation::DeleteJob:
                return service.delete_job({.job_id = job.id, .expected_revision = job.revision});
            case Mutation::RunNow:
                return discard_value(service.run_now({.job_id = job.id, .idempotency_key = "manual"}));
        }
        FAIL("unhandled mutation");
        return Result<void, Error>::success();
    }
};

auto mutation_writes(Mutation mutation) -> std::vector<std::string>
{
    switch (mutation) {
        case Mutation::CreateQueue:
            return {"queue.insert", "idempotency.insert"};
        case Mutation::UpdateQueue:
        case Mutation::SuspendQueue:
        case Mutation::ResumeQueue:
            return {"queue.update"};
        case Mutation::DeleteQueue:
            return {"references.delete", "job.update", "run.update", "queue.update"};
        case Mutation::CreateJob:
        case Mutation::CreateImmediateJob:
            return {"job.insert", "references.delete", "references.insert", "run.insert", "idempotency.insert"};
        case Mutation::UpdateJob:
            return {"job.update", "references.delete", "references.insert", "run.update"};
        case Mutation::SuspendJob:
        case Mutation::ResumeJob:
            return {"job.update"};
        case Mutation::MoveJob:
            return {"job.update", "run.update"};
        case Mutation::DeleteJob:
            return {"job.update", "run.update", "references.delete"};
        case Mutation::RunNow:
            return {"run.insert", "idempotency.insert"};
    }
    FAIL("unhandled mutation");
    return {};
}

auto mutation_faults(Mutation mutation) -> std::vector<DatabaseCall>
{
    std::vector<DatabaseCall> result{
        {.boundary = "connection", .operation = Operation::Begin },
        {.boundary = "connection", .operation = Operation::Commit}
    };
    for (auto operation : {Operation::Prepare, Operation::Execute, Operation::Fetch}) {
        result.push_back({.boundary = "read", .operation = operation});
    }
    if (mutation == Mutation::CreateJob || mutation == Mutation::UpdateJob) {
        for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute, Operation::Fetch}) {
            result.push_back({.boundary = "secret.read", .operation = operation});
        }
    }
    for (auto const& write : mutation_writes(mutation)) {
        for (auto operation : {Operation::Prepare, Operation::Bind, Operation::Execute}) {
            result.push_back({.boundary = write, .operation = operation});
        }
        result.push_back({.boundary = write, .operation = Operation::Execute, .phase = Phase::AfterSuccess});
    }
    return result;
}

auto read(Fixture& fixture, std::size_t index) -> Result<void, Error>
{
    switch (index) {
        case 0:
            return discard_value(fixture.service.get_queue(recovery_id(1)));
        case 1:
            return discard_value(fixture.service.list_queues({}));
        case 2:
            return discard_value(fixture.service.get_job(recovery_id(2)));
        default:
            return discard_value(fixture.service.list_jobs({}));
    }
}

} // namespace

TEST_CASE("Management name conflicts with successful rollback keep mutation admission open",
          "[jobu][management][fault][sqlite]")
{
    Fixture fixture;
    auto    queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);

    auto conflict = fixture.service.create_queue({.name = queue.name});
    REQUIRE_FALSE(conflict);
    CHECK(conflict.error().code == "jobu.queue.name_conflict");
    CHECK(fixture.failures.empty());
    CHECK(fixture.committed == 0U);
    REQUIRE(fixture.service.create_queue({.name = "later"}));
    CHECK(fixture.failures.empty());
    CHECK(fixture.committed == 1U);
}

TEST_CASE("Management write and rollback failures preserve the first storage error",
          "[jobu][management][fault][sqlite]")
{
    Fixture fixture;
    // Fail after SQLite inserted the row, leaving real work for the failing rollback to undo.
    fixture.arm({.boundary = "queue.insert", .operation = Operation::Execute, .phase = Phase::AfterSuccess}, "db.io");
    fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
    auto result = fixture.service.create_queue({.name = "new-queue"});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "db.io");
    fixture.require_faults_fired();
    fixture.check_terminal_failure("db.io");
    fixture.check_closed_gate();
    fixture.check_poisoned_connection();

    // Closing discards SQLite's still-open transaction; reopening must not reveal the inserted queue.
    fixture.storage.reopen();
    auto missing = fixture.service.get_queue(recovery_id(100));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "jobu.queue.not_found");
}

TEST_CASE("Management conflict followed by rollback failure closes mutation admission immediately",
          "[jobu][management][fault][sqlite]")
{
    Fixture fixture;
    auto    queue = recovery_queue(recovery_id(1));
    fixture.storage.insert_queue(queue);

    // The ordinary conflict is safe only if its transaction unwinds successfully. A poisoned
    // connection must notify the runtime now, without waiting for another request to discover it.
    fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
    auto result = fixture.service.create_queue({.name = queue.name});
    REQUIRE_FALSE(result);
    fixture.require_faults_fired();
    fixture.check_terminal_failure("db.rollback_failed");
    fixture.check_closed_gate();
    fixture.check_poisoned_connection();

    fixture.storage.reopen();
    auto original = fixture.service.get_queue(queue.id);
    REQUIRE(original);
    CHECK(original->name == queue.name);
}

TEST_CASE("Every management mutation rolls back storage faults before its once-only fatal notification",
          "[jobu][management][fault][sqlite]")
{
    for (auto mutation : mutations) {
        for (auto const& fault : mutation_faults(mutation)) {
            DYNAMIC_SECTION(static_cast<int>(mutation)
                            << ' ' << fault.boundary << ' ' << static_cast<int>(fault.operation) << ' '
                            << static_cast<int>(fault.phase))
            {
                MutationFixture fixture{mutation};
                auto            before = storage_snapshot(fixture.storage.database);
                fixture.arm(fault);
                auto failed = fixture.mutate(mutation);
                REQUIRE_FALSE(failed);
                check_safe_error(failed.error(), "db.io");
                fixture.require_faults_fired();
                if (fault.operation == Operation::Begin) {
                    REQUIRE(fixture.failures.size() == 1U);
                    CHECK(fixture.calls_at_failure.back() == fault);
                }
                else {
                    fixture.check_terminal_failure("db.io", false);
                }
                fixture.check_closed_gate();
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);
            }
        }
    }
}

TEST_CASE("Management lost commit acknowledgements preserve the entire successful mutation",
          "[jobu][management][fault][sqlite]")
{
    for (auto mutation : mutations) {
        DYNAMIC_SECTION(static_cast<int>(mutation))
        {
            // The same deterministic request supplies a successful control for every persisted column,
            // including refreshed snapshots, tombstones, secret references and idempotency records.
            MutationFixture control{mutation};
            auto            before = storage_snapshot(control.storage.database);
            REQUIRE(control.mutate(mutation));
            CHECK(control.committed == 1U);
            CHECK(control.failures.empty());
            control.storage.reopen();
            auto committed = storage_snapshot(control.storage.database);
            CHECK(committed != before);

            MutationFixture fixture{mutation};
            fixture.arm({.boundary = "connection", .operation = Operation::Commit, .phase = Phase::AfterSuccess});
            auto failed = fixture.mutate(mutation);
            REQUIRE_FALSE(failed);
            check_safe_error(failed.error(), "db.io");
            fixture.require_faults_fired();
            fixture.check_terminal_failure("db.io");
            fixture.check_closed_gate();
            fixture.storage.reopen();
            CHECK(storage_snapshot(fixture.storage.database) == committed);
        }
    }
}

TEST_CASE("Every management read distinguishes ordinary storage errors from corruption on SQLite",
          "[jobu][management][fault][sqlite]")
{
    for (std::size_t index = 0; index < 4; ++index) {
        for (auto operation : {Operation::Prepare, Operation::Execute, Operation::Fetch}) {
            for (bool corrupt : {false, true}) {
                DYNAMIC_SECTION(index << ' ' << static_cast<int>(operation) << " corrupt=" << corrupt)
                {
                    MutationFixture fixture{Mutation::UpdateJob};
                    auto const*     code = corrupt ? "db.corrupt" : "db.io";
                    fixture.arm({.boundary = "read", .operation = operation}, code);
                    auto failed = read(fixture, index);
                    REQUIRE_FALSE(failed);
                    check_safe_error(failed.error(), code);
                    fixture.require_faults_fired();
                    CHECK(fixture.committed == 0U);
                    if (corrupt) {
                        REQUIRE(fixture.failures.size() == 1U);
                        CHECK(fixture.failures.front() == failed.error());
                        fixture.check_closed_gate();
                        // Reads remain callable, but another corruption must not emit a second terminal event.
                        fixture.arm({.boundary = "read", .operation = operation}, code);
                        REQUIRE_FALSE(read(fixture, index));
                        fixture.require_faults_fired();
                        CHECK(fixture.failures.size() == 1U);
                    }
                    else {
                        CHECK(fixture.failures.empty());
                        REQUIRE(read(fixture, index));
                        REQUIRE(fixture.mutate(Mutation::UpdateJob));
                        CHECK(fixture.committed == 1U);
                        CHECK(fixture.failures.empty());
                    }
                }
            }
        }
    }
}

TEST_CASE("Management revision conflicts stay nonfatal and unexpected constraints fail closed",
          "[jobu][management][fault][sqlite]")
{
    MutationFixture fixture{Mutation::UpdateJob};
    auto            before   = storage_snapshot(fixture.storage.database);
    auto            conflict = fixture.service.update_job(
        {.job_id = fixture.job.id, .expected_revision = fixture.job.revision + 1, .priority = 3});
    REQUIRE_FALSE(conflict);
    CHECK(conflict.error().code == "jobu.job.revision_conflict");
    CHECK(fixture.failures.empty());
    CHECK(fixture.committed == 0U);
    CHECK(storage_snapshot(fixture.storage.database) == before);

    fixture.arm({.boundary = "job.update", .operation = Operation::Execute}, "db.constraint");
    auto failed = fixture.mutate(Mutation::UpdateJob);
    REQUIRE_FALSE(failed);
    fixture.require_faults_fired();
    check_safe_error(failed.error(), "db.constraint");
    fixture.check_terminal_failure("db.constraint", false);
    fixture.check_closed_gate();
    fixture.storage.reopen();
    CHECK(storage_snapshot(fixture.storage.database) == before);
}

TEST_CASE("Missing job secret names stay ordinary unless rollback poisons the connection", "[jobu][management][fault]")
{
    for (bool create : {false, true}) {
        for (bool poison : {false, true}) {
            DYNAMIC_SECTION("create=" << create << " poison=" << poison)
            {
                MutationFixture fixture{Mutation::UpdateJob};
                auto payload = parse_json(R"({"command":"/bin/tool","arguments":[{"secret":"missing.token"}]})");
                REQUIRE(payload);
                auto before = storage_snapshot(fixture.storage.database);
                if (poison) {
                    fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
                }
                auto result = create
                                ? fixture.service.create_job({.queue    = fixture.queue.id,
                                                              .schedule = std::get<OnceSchedule>(fixture.job.schedule),
                                                              .payload  = *payload,
                                                              .idempotency_key = "missing"})
                                : fixture.service.update_job(
                                      {.job_id = fixture.job.id, .expected_revision = 1, .payload = *payload});
                REQUIRE_FALSE(result);
                if (poison) {
                    fixture.require_faults_fired();
                    fixture.check_terminal_failure("db.rollback_failed");
                    fixture.check_closed_gate();
                    fixture.check_poisoned_connection();
                }
                else {
                    check_safe_error(result.error(), "jobu.secret.not_found");
                    CHECK(result.error().category == ErrorCategory::NotFound);
                    CHECK(fixture.failures.empty());
                    CHECK(fixture.committed == 0);
                    CHECK(storage_snapshot(fixture.storage.database) == before);
                }
                fixture.storage.reopen();
                CHECK(storage_snapshot(fixture.storage.database) == before);
            }
        }
    }
}

TEST_CASE("Reference replacement failure retains its fatal cause when rollback also fails", "[jobu][management][fault]")
{
    MutationFixture fixture{Mutation::UpdateJob};
    auto            before = storage_snapshot(fixture.storage.database);
    fixture.arm({.boundary = "references.insert", .operation = Operation::Execute, .phase = Phase::AfterSuccess});
    fixture.arm({.boundary = "connection", .operation = Operation::Rollback}, "db.rollback_failed");
    auto result = fixture.mutate(Mutation::UpdateJob);
    REQUIRE_FALSE(result);
    check_safe_error(result.error(), "db.io");
    fixture.require_faults_fired();
    fixture.check_terminal_failure("db.io");
    fixture.check_closed_gate();
    fixture.check_poisoned_connection();
    fixture.storage.reopen();
    CHECK(storage_snapshot(fixture.storage.database) == before);
}
