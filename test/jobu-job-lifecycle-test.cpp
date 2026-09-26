#include "job_lifecycle_priv.hpp"

#include "domain_storage_priv.hpp"
#include "job_repository_priv.hpp"
#include "query.hpp"
#include "run_repository_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/recovery_fixture.hpp"
#include "transaction.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto at(std::int64_t seconds) -> UtcTimePoint
{
    return UtcTimePoint{std::chrono::seconds{seconds}};
}

auto load_job(RecoveryFixture& fixture, Uuid const& id) -> JobDefinition
{
    JobRepository jobs{fixture.database, fixture.registry};
    auto          found = jobs.find_by_id(id, true);
    REQUIRE(found);
    REQUIRE(found->has_value());
    return std::move(**found);
}

void execute_for_run(Database& database, std::string_view sql, Uuid const& run_id)
{
    Query query{database};
    REQUIRE(query.prepare(sql));
    REQUIRE(query.bind_value(":run_id", uuid_to_storage(run_id)));
    REQUIRE(query.exec());
}

void require_invariant(auto const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.storage.invariant");
}

enum class RelationshipCase : std::uint8_t {
    UnfinishedWithoutWork,
    TerminalWithoutHistory,
    TerminalWithLiveManual,
    OnceManualOnly,
    CronWithoutWork,
    CronManualOnly,
    DuplicateManual
};

enum class ReconcileErrorCase : std::uint8_t {
    MissingRun,
    NonterminalRun,
    MissingResult,
    ExhaustedRevision,
    TerminalOwnerWithLiveManual
};

} // namespace

TEST_CASE("The last one-time run determines the durable job outcome", "[jobu][lifecycle][sqlite]")
{
    auto const run_state = GENERATE(RunState::Succeeded, RunState::Failed, RunState::Interrupted, RunState::Cancelled);
    auto const initial_state = GENERATE(JobState::Active, JobState::Suspending);
    CAPTURE(run_state, initial_state);

    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    job.state             = initial_state;
    job.revision          = 4;
    fixture.insert_queue(queue);
    fixture.insert_job(job);

    auto terminal             = fixture.make_run(recovery_id(3), job, run_state);
    terminal.run.job_revision = 2; // Suspension or another control change may advance the current definition.
    fixture.insert_run(terminal);

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    auto finished    = lifecycle.finish_after_terminal_run(terminal.run.id, at(30));
    REQUIRE(finished);
    REQUIRE(*finished);
    REQUIRE(transaction.commit());

    auto saved    = load_job(fixture, job.id);
    auto expected = JobState::Failed;
    if (run_state == RunState::Succeeded) {
        expected = JobState::Succeeded;
    }
    else if (run_state == RunState::Cancelled) {
        expected = JobState::Cancelled;
    }
    CHECK(saved.state == expected);
    CHECK(saved.revision == 5);
    CHECK(saved.updated_at == at(30));
    fixture.require_run(terminal); // The immutable run snapshot retains revision 2.

    auto replay = Transaction::begin(fixture.database);
    REQUIRE(replay);
    auto replay_transaction = std::move(*replay);
    auto unchanged          = lifecycle.finish_after_terminal_run(terminal.run.id, at(40));
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
    REQUIRE(replay_transaction.commit());
    CHECK(load_job(fixture, job.id).updated_at == at(30));
}

TEST_CASE("Other live work keeps a one-time definition unfinished", "[jobu][lifecycle][sqlite]")
{
    auto const      final_run_is_manual = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);

    auto final_origin = final_run_is_manual ? RunOrigin::Manual : RunOrigin::Scheduled;
    auto live_origin  = final_run_is_manual ? RunOrigin::Scheduled : RunOrigin::Manual;
    auto terminal     = fixture.make_run(recovery_id(3), job, RunState::Succeeded, 0, final_origin);
    auto live         = fixture.make_run(recovery_id(4), job, RunState::Scheduled, 0, live_origin);
    fixture.insert_run(terminal);
    fixture.insert_run(live);

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    auto finished    = lifecycle.finish_after_terminal_run(terminal.run.id, at(30));
    REQUIRE(finished);
    CHECK_FALSE(*finished);
    REQUIRE(transaction.commit());
    CHECK(load_job(fixture, job.id).state == JobState::Active);
    CHECK(load_job(fixture, job.id).revision == 1);
    REQUIRE(lifecycle.validate_job_lifecycle(load_job(fixture, job.id)));
}

TEST_CASE("The final manual result wins after the original occurrence was cancelled", "[jobu][lifecycle][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Cancelled));
    auto manual = fixture.make_run(recovery_id(4), job, RunState::Succeeded, 0, RunOrigin::Manual);
    fixture.insert_run(manual);

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    auto finished    = lifecycle.finish_after_terminal_run(manual.run.id, at(30));
    REQUIRE(finished);
    REQUIRE(*finished);
    REQUIRE(transaction.commit());
    CHECK(load_job(fixture, job.id).state == JobState::Succeeded);
}

TEST_CASE("Deleted and recurring definitions retain their state", "[jobu][lifecycle][sqlite]")
{
    auto const      recurring = GENERATE(false, true);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    if (recurring) {
        job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    }
    else {
        job.state      = JobState::Deleted;
        job.deleted_at = at(3);
    }
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    auto terminal = fixture.make_run(recovery_id(3), job, RunState::Failed);
    fixture.insert_run(terminal);

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    auto finished    = lifecycle.finish_after_terminal_run(terminal.run.id, at(30));
    REQUIRE(finished);
    CHECK_FALSE(*finished);
    REQUIRE(transaction.commit());
    CHECK(load_job(fixture, job.id).state == job.state);
    CHECK(load_job(fixture, job.id).revision == job.revision);
}

TEST_CASE("Lifecycle validation checks one-time, recurring, and terminal relationships", "[jobu][lifecycle][sqlite]")
{
    auto const shape = GENERATE(RelationshipCase::UnfinishedWithoutWork,
                                RelationshipCase::TerminalWithoutHistory,
                                RelationshipCase::TerminalWithLiveManual,
                                RelationshipCase::OnceManualOnly,
                                RelationshipCase::CronWithoutWork,
                                RelationshipCase::CronManualOnly,
                                RelationshipCase::DuplicateManual);
    CAPTURE(shape);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    if (shape == RelationshipCase::TerminalWithoutHistory) {
        job.state = JobState::Succeeded;
    }
    if (shape == RelationshipCase::TerminalWithLiveManual) {
        job.state = JobState::Failed;
    }
    if (shape == RelationshipCase::CronWithoutWork || shape == RelationshipCase::CronManualOnly) {
        job.schedule = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    }
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    if (shape == RelationshipCase::TerminalWithLiveManual || shape == RelationshipCase::OnceManualOnly ||
        shape == RelationshipCase::CronManualOnly || shape == RelationshipCase::DuplicateManual) {
        fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Scheduled, 0, RunOrigin::Manual));
    }
    if (shape == RelationshipCase::DuplicateManual) {
        fixture.insert_run(fixture.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual));
    }

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   validated = lifecycle.validate_job_lifecycle(load_job(fixture, job.id));
    if (shape == RelationshipCase::TerminalWithoutHistory || shape == RelationshipCase::OnceManualOnly ||
        shape == RelationshipCase::CronWithoutWork) {
        REQUIRE(validated); // Terminal history may be gone; recurring 0/0 awaits its existing repair path.
    }
    else {
        require_invariant(validated);
    }
}

TEST_CASE("Lifecycle reconciliation rejects bad input and exhausted revisions", "[jobu][lifecycle][sqlite]")
{
    auto const scenario = GENERATE(ReconcileErrorCase::MissingRun,
                                   ReconcileErrorCase::NonterminalRun,
                                   ReconcileErrorCase::MissingResult,
                                   ReconcileErrorCase::ExhaustedRevision,
                                   ReconcileErrorCase::TerminalOwnerWithLiveManual);
    CAPTURE(scenario);
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    if (scenario == ReconcileErrorCase::ExhaustedRevision) {
        job.revision = static_cast<JobRevision>(std::numeric_limits<std::int64_t>::max());
    }
    if (scenario == ReconcileErrorCase::TerminalOwnerWithLiveManual) {
        job.state = JobState::Succeeded;
    }
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    if (scenario != ReconcileErrorCase::MissingRun) {
        auto run_state = scenario == ReconcileErrorCase::NonterminalRun ? RunState::Scheduled : RunState::Succeeded;
        fixture.insert_run(fixture.make_run(recovery_id(3), job, run_state));
    }
    if (scenario == ReconcileErrorCase::TerminalOwnerWithLiveManual) {
        fixture.insert_run(fixture.make_run(recovery_id(4), job, RunState::Scheduled, 0, RunOrigin::Manual));
    }
    if (scenario == ReconcileErrorCase::MissingResult) {
        // A terminal row without its result violates the reconciliation precondition.
        execute_for_run(fixture.database, "UPDATE jobu_runs SET result_json = NULL WHERE id = :run_id", recovery_id(3));
    }

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    auto result      = lifecycle.finish_after_terminal_run(recovery_id(3), at(30));
    if (scenario == ReconcileErrorCase::ExhaustedRevision) {
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.job.revision_exhausted");
    }
    else {
        require_invariant(result);
    }
    REQUIRE(transaction.rollback());
    CHECK(load_job(fixture, job.id).revision == job.revision);
}

TEST_CASE("A caller rollback restores both the final run and job state", "[jobu][lifecycle][sqlite]")
{
    RecoveryFixture fixture;
    auto            queue = recovery_queue(recovery_id(1));
    auto            job   = fixture.make_job(recovery_id(2), queue.id);
    fixture.insert_queue(queue);
    fixture.insert_job(job);
    fixture.insert_run(fixture.make_run(recovery_id(3), job, RunState::Running));

    JobLifecycleRepository lifecycle{fixture.database, fixture.registry};
    auto                   begun = Transaction::begin(fixture.database);
    REQUIRE(begun);
    auto transaction = std::move(*begun);
    // Model the caller's terminal run write in the same transaction as reconciliation.
    execute_for_run(fixture.database,
                    "UPDATE jobu_runs SET state = 'succeeded', completed_at_us = 30000000, "
                    "result_json = '{}' WHERE id = :run_id",
                    recovery_id(3));
    auto finished = lifecycle.finish_after_terminal_run(recovery_id(3), at(30));
    REQUIRE(finished);
    REQUIRE(*finished);
    REQUIRE(transaction.rollback());

    CHECK(load_job(fixture, job.id).state == JobState::Active);
    CHECK(load_job(fixture, job.id).revision == 1);
    RunRepository runs{fixture.database, fixture.registry};
    auto          found = runs.find_by_id(recovery_id(3));
    REQUIRE(found);
    REQUIRE(found->has_value());
    CHECK((*found)->state == RunState::Running);
}
