/// @file recovery_fixture.hpp
/// @brief Reusable persisted-state fixtures for synchronous Phase 7 tests.
#pragma once

#include "attempt_repository_priv.hpp"
#include "attribute_registry.hpp"
#include "database.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "run.hpp"
#include "temporary_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jb::test {

/// One attempt and its optional capture row; absent output differs from empty captured bytes.
struct RecoveryAttemptFixture {
    jobu::JobAttempt                           attempt;
    std::optional<jobu::detail::AttemptOutput> output;
};

/// Owning expected snapshot and ordered history, independent of the live database.
struct RecoveryRunFixture {
    jobu::JobRun                        run;
    std::vector<RecoveryAttemptFixture> attempts;
};

/// Produces stable UUIDv7-shaped identities, with enough ordered suffix space for paginated scans.
[[nodiscard]] auto recovery_id(std::uint32_t suffix) -> core::Uuid;

/// Builds an owner at a fixed time; Deleted includes the required tombstone timestamp.
[[nodiscard]] auto recovery_queue(core::Uuid           id,
                                  jobu::QueueState     state  = jobu::QueueState::Active,
                                  jobu::RecoveryPolicy policy = jobu::RecoveryPolicy::FailInterrupted) -> jobu::Queue;

/// Owns a real SQLite database and its directory on the calling test thread.
///
/// Builders use fixed times, materialized attributes and valid inert payloads; no runner is invoked. Persistence
/// helpers seed rows, not management operations, and do not certify cross-row validity. Deliberately inconsistent
/// scenarios must modify these values or execute explicit SQL in the test. Helpers report failures through Catch2.
class RecoveryFixture final {
public:
    /// Optionally decorates the closed SQLite driver; the wrapper must preserve exclusive ownership and file identity.
    explicit RecoveryFixture(std::function<std::unique_ptr<db::Driver>(std::unique_ptr<db::Driver>)> wrap_driver = {});

    /// Builds an active one-time definition with three allowed attempts; callers may edit the returned value.
    [[nodiscard]] auto make_job(core::Uuid id, core::Uuid queue_id, jobu::JobType type = jobu::JobType::Cli) const
        -> jobu::JobDefinition;

    /// Builds a snapshot and contiguous history, with no output by default.
    ///
    /// Prior failures precede the current attempt. RetryWait requires at least one prior failure. Cancelled represents
    /// cancellation while waiting (zero attempts, or completed failures); Interrupted represents recovery's
    /// terminal shape. Scheduled requires zero prior failures. Only Scheduled/Manual
    /// origins are supported, with at most 998 prior failures. The caller must choose a retry allowance consistent
    /// with the requested history; overflow scenarios should edit a small history explicitly.
    [[nodiscard]] auto make_run(core::Uuid                 id,
                                jobu::JobDefinition const& job,
                                jobu::RunState             state          = jobu::RunState::Scheduled,
                                jobu::AttemptNumber        prior_failures = 0,
                                jobu::RunOrigin origin = jobu::RunOrigin::Scheduled) const -> RecoveryRunFixture;

    /// Persists an owner, including its tombstone representation when deleted.
    void insert_queue(jobu::Queue const& queue);
    void insert_job(jobu::JobDefinition const& job);

    /// Seeds one complete history atomically using existing serializers; allows storage-valid manual terminal rows.
    void insert_run(RecoveryRunFixture const& expected);

    /// Closes and reopens the same database; callers must release all queries and transactions first.
    void reopen();

    /// Reads through production repositories and compares every run, attempt and output field, including row absence.
    void require_run(RecoveryRunFixture const& expected);

    TemporaryDirectory              directory;
    std::filesystem::path           database_file{directory.path() / "recovery.sqlite"};
    db::Database                    database;
    jobu::StandardAttributeRegistry registry;
};

} // namespace jb::test
