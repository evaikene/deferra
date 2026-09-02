/** @file management.hpp
 * @brief Defines the synchronous JobU management service and queue/job requests.
 */
#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "job.hpp"
#include "json.hpp"
#include "object.hpp"
#include "queue.hpp"
#include "result.hpp"
#include "run.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class CronEngine;

/// Selects a queue by stable UUID or exact user-facing name.
using QueueSelector = std::variant<jb::core::Uuid, std::string>;

/// Shared bounded keyset-pagination options for management lists.
struct PageOptions {
    /// Number of resources to return; accepted values are 1 through 200.
    std::size_t                   limit{100};
    /// Return resources whose UUID bytes sort after this ID.
    std::optional<jb::core::Uuid> after_id;
};

/// One bounded page of queues ordered by ascending UUID bytes.
struct QueuePage {
    /// Queues in deterministic UUID order.
    std::vector<Queue>            items;
    /// ID of the last returned queue when another page exists.
    std::optional<jb::core::Uuid> next_after_id;
};

/// One bounded page of job definitions ordered by ascending UUID bytes.
struct JobPage {
    /// Job definitions in deterministic UUID order.
    std::vector<JobDefinition>    items;
    /// ID of the last returned job when another page exists.
    std::optional<jb::core::Uuid> next_after_id;
};

/// Values used to create an active queue.
struct CreateQueueRequest {
    /// User-facing name containing 1 through 128 bytes of valid UTF-8.
    /// ASCII control characters and reserved deletion suffixes are rejected.
    std::string                         name;
    /// Positive scheduler weight.
    std::uint32_t                       weight{1};
    /// Positive combined concurrency limit.
    std::uint32_t                       concurrency_limit{1};
    /// Startup policy for interrupted attempts.
    RecoveryPolicy                      recovery_policy{RecoveryPolicy::FailInterrupted};
    /// Partial queue-default attribute layer.
    AttributeSet                        defaults;
    /// Retention override, zero for unlimited, or no value to inherit daemon policy.
    std::optional<std::chrono::seconds> history_retention;
    /// Nonnegative delay before runnable-work warnings.
    std::chrono::milliseconds           runnable_wait_warning{10000};
    /// Optional 1-through-128-byte UTF-8 key reserved for durable create replay.
    std::optional<std::string>          idempotency_key;
};

/** Values used to replace selected mutable queue fields.
 *
 * Omitted fields remain unchanged. Supplying an empty AttributeSet clears all queue defaults. The nested retention
 * optional distinguishes unchanged, inherit daemon policy, and a concrete zero/unlimited or positive duration.
 */
struct UpdateQueueRequest {
    /// Existing non-deleted queue to update.
    QueueSelector                                      queue;
    /// Replacement user-facing name.
    std::optional<std::string>                         name;
    /// Replacement positive scheduler weight.
    std::optional<std::uint32_t>                       weight;
    /// Replacement positive combined concurrency limit.
    std::optional<std::uint32_t>                       concurrency_limit;
    /// Replacement startup recovery policy.
    std::optional<RecoveryPolicy>                      recovery_policy;
    /// Complete replacement partial queue-default layer.
    std::optional<AttributeSet>                        defaults;
    /// Unchanged, inherit daemon policy, or a concrete retention duration.
    std::optional<std::optional<std::chrono::seconds>> history_retention;
    /// Replacement nonnegative runnable-work warning delay.
    std::optional<std::chrono::milliseconds>           runnable_wait_warning;
};

/// Filters and bounds for listing queues.
struct QueueListRequest {
    /// Includes soft-deleted queues when true.
    bool                      include_deleted{false};
    /// Restricts results to one lifecycle state when supplied.
    std::optional<QueueState> state;
    /// UUID keyset and requested page size.
    PageOptions               page;
};

/** Values used to create one active job and its first scheduled run.
 *
 * Cron schedules are validated and evaluated by ManagementService. The supplied job attribute layer is materialized
 * over built-in, daemon, and resolved queue defaults before persistence. The payload remains owning and preserves
 * unknown additive members.
 */
struct CreateJobRequest {
    /// Existing non-deleted queue selected by UUID or exact user-facing name.
    QueueSelector              queue;
    /// Optional non-unique display name containing at most 256 valid UTF-8 bytes and no ASCII controls.
    std::optional<std::string> name;
    /// Runner family whose structural payload rules are applied.
    JobType                    type{JobType::Cli};
    /// One-time schedule or recurring cron schedule used to plan the first occurrence.
    JobSchedule                schedule;
    /// Signed scheduling priority copied into the run snapshot.
    std::int32_t               priority{0};
    /// Partial job-specific attribute layer.
    AttributeSet               attributes;
    /// Owning CLI or HTTP payload object, limited to 256 KiB when deterministically encoded.
    jb::core::JsonValue        payload;
    /// Optional 1-through-128-byte UTF-8 key reserved for durable create replay.
    std::optional<std::string> idempotency_key;
};

/** Changes selected fields of one durable job definition using optimistic concurrency.
 *
 * The name's outer optional distinguishes unchanged from changed, while its inner optional distinguishes clearing the
 * name from replacing it. Attribute changes patch the stored complete materialized set; omitted attributes remain
 * unchanged and no daemon or queue defaults are reapplied. Recurring changes refresh an unstarted scheduled occurrence
 * or apply only to the definition while its current occurrence is running or waiting to retry.
 */
struct UpdateJobRequest {
    /// Stable job-definition UUID to update.
    jb::core::Uuid                            job_id;
    /// Positive revision that must match the durable definition.
    JobRevision                               expected_revision{0};
    /// Unchanged name, cleared name, or replacement display name.
    std::optional<std::optional<std::string>> name;
    /// Replacement runner family, validated together with the resulting payload.
    std::optional<JobType>                    type;
    /// Replacement one-time or recurring schedule; conversion requires a scheduled current occurrence with no attempt.
    std::optional<JobSchedule>                schedule;
    /// Replacement scheduling priority.
    std::optional<std::int32_t>               priority;
    /// Job-scope values that replace the corresponding stored materialized values.
    AttributeSet                              attribute_changes;
    /// Replacement owning runner payload.
    std::optional<jb::core::JsonValue>        payload;
};

/// Filters and bounds for listing job definitions.
struct JobListRequest {
    /// Optional queue selector. Deleted queues are visible only when include_deleted is true.
    std::optional<QueueSelector> queue;
    /// Includes soft-deleted job definitions when true.
    bool                         include_deleted{false};
    /// Restricts results to one lifecycle state when supplied.
    std::optional<JobState>      state;
    /// Restricts results to one runner family when supplied.
    std::optional<JobType>       type;
    /// UUID keyset and requested page size.
    PageOptions                  page;
};

/// Values used to move one suspended job to a different queue using optimistic concurrency.
struct MoveJobRequest {
    /// Stable job-definition UUID to move.
    jb::core::Uuid job_id;
    /// Positive revision that must match the durable definition.
    JobRevision    expected_revision{0};
    /// Existing active or suspended destination queue selected by UUID or exact user-facing name.
    QueueSelector  target_queue;
};

/// Values used to soft-delete one suspended job using optimistic concurrency.
struct DeleteJobRequest {
    /// Stable job-definition UUID to delete.
    jb::core::Uuid job_id;
    /// Positive revision that must match the durable definition.
    JobRevision    expected_revision{0};
};

/** Values used to create one immediate manual occurrence of a job definition.
 *
 * Run Now preserves the definition's existing schedule-owned occurrence. The optional idempotency key is scoped to
 * the selected job and durably replays the original manual run.
 */
struct RunNowRequest {
    /// Existing non-deleted job definition whose current execution values are snapshotted.
    jb::core::Uuid             job_id;
    /// Optional 1-through-128-byte UTF-8 key reserved for durable Run Now replay.
    std::optional<std::string> idempotency_key;
};

/** Synchronous owner-thread Object for durable JobU management operations.
 *
 * The service borrows an already-open Database, AttributeRegistry, CronEngine, UuidGenerator, and TimeSource; each
 * collaborator must outlive the service. Construct the service and perform every operation on the shared Object and
 * Database owner thread. Passing a parent transfers normal `jb::core::Object` lifetime ownership but does not transfer
 * ownership of any borrowed collaborator. The daemon-default attribute layer is validated and copied during
 * construction; a validation failure is returned by subsequent operations.
 *
 * Queue/job gets and lists use bounded repository reads without an explicit transaction. Creates, updates, lifecycle
 * changes, moves, and deletions use one immediate transaction and return only after commit. Errors include stable
 * `jobu.queue.*`, `jobu.job.*`, `jobu.run.*`, `jobu.schedule.*`, `jobu.attribute.*`, and `jobu.idempotency.*` codes
 * plus unchanged database errors when no domain mapping applies. Methods invoke no callbacks, start no threads, and
 * perform no event-loop processing. A fresh recurring create or an update that validates or evaluates a recurring
 * schedule may synchronously load timezone data through CronEngine.
 */
class ManagementService final : public jb::core::Object {
public:
    /** Constructs a management service by borrowing its collaborators.
     * @param database Already-open sole JobU database connection.
     * @param attributes Registry used to validate and decode durable attributes.
     * @param cron Owner-thread cron validator and occurrence calculator used synchronously by recurring operations.
     * @param uuid_generator Generator used for new durable identities.
     * @param time_source Source used for durable UTC timestamps.
     * @param daemon_defaults Partial daemon-default attribute layer to validate and copy.
     * @param parent Optional Object that owns this service and supplies its event-loop affinity.
     * @warning Every argument and the constructor call itself belong to the Database owner thread.
     */
    ManagementService(jb::db::Database&        database,
                      AttributeRegistry const& attributes,
                      CronEngine const&        cron,
                      jb::core::UuidGenerator& uuid_generator,
                      jb::core::TimeSource&    time_source,
                      AttributeSet             daemon_defaults = {},
                      jb::core::Object*        parent          = nullptr);

    /// Destroys private repositories without changing the borrowed Database state.
    ~ManagementService() override;

    /// Prevents copying borrowed collaborators and repository state.
    ManagementService(ManagementService const&)                    = delete;
    /// Prevents moving a service whose repositories borrow a fixed Database.
    ManagementService(ManagementService&&)                         = delete;
    /// Prevents copy assignment of borrowed collaborators and repository state.
    auto operator=(ManagementService const&) -> ManagementService& = delete;
    /// Prevents move assignment of repositories that borrow a fixed Database.
    auto operator=(ManagementService&&) -> ManagementService&      = delete;

    /** Creates one active queue in an immediate transaction.
     * @param request Queue configuration consumed after validation. An optional idempotency key durably replays the
     * original successful result when the normalized request matches.
     * @return Committed or replayed queue, or a validation, idempotency, name-conflict, generator, attribute, or
     * database Error.
     */
    [[nodiscard]] auto create_queue(CreateQueueRequest request) -> jb::core::Result<Queue, jb::core::Error>;

    /** Gets one queue by exact ID or exact name without starting an explicit transaction.
     * @param selector Queue UUID or user-facing name. Name lookup prefers a non-deleted queue.
     * @param include_deleted Permits ID lookup of deleted queues and historical original-name lookup. Ambiguous
     * historical names return `jobu.queue.ambiguous_deleted_name`.
     * @return Queue value, `jobu.queue.not_found`, or a validation/storage/database Error.
     */
    [[nodiscard]] auto get_queue(QueueSelector const& selector, bool include_deleted = false)
        -> jb::core::Result<Queue, jb::core::Error>;

    /** Lists a bounded page using ascending UUID-byte keyset order.
     * @param request Optional state/deletion filters and a limit from 1 through 200.
     * @return At most the requested number of queues and a cursor only when another row exists, or an Error.
     */
    [[nodiscard]] auto list_queues(QueueListRequest const& request) -> jb::core::Result<QueuePage, jb::core::Error>;

    /** Replaces supplied mutable fields in one immediate transaction.
     * @param request Non-empty update consumed after validation and targeting an existing non-deleted queue.
     * @return Committed replacement queue, or a validation, not-found, name-conflict, state, attribute, or database
     * Error. Queue-default changes affect only jobs created later.
     */
    [[nodiscard]] auto update_queue(UpdateQueueRequest request) -> jb::core::Result<Queue, jb::core::Error>;

    /** Suspends a queue without changing its jobs or runs.
     * @param selector Existing queue UUID or exact user-facing name, borrowed only for this call.
     * @return Committed queue. Active queues pass through suspending and complete immediately when no running work is
     * present; suspending queues complete when running work has drained. Suspended queues are returned unchanged.
     * Deleted, not-found, state-conflict, and database failures are reported as Error values.
     */
    [[nodiscard]] auto suspend_queue(QueueSelector const& selector) -> jb::core::Result<Queue, jb::core::Error>;

    /** Resumes a suspending or suspended queue without changing its jobs or runs.
     * @param selector Existing queue UUID or exact user-facing name, borrowed only for this call.
     * @return Committed active queue, the unchanged queue when already active, or a deleted, not-found,
     * state-conflict, or database Error.
     */
    [[nodiscard]] auto resume_queue(QueueSelector const& selector) -> jb::core::Result<Queue, jb::core::Error>;

    /** Soft-deletes a fully suspended queue and all of its non-deleted jobs in one immediate transaction.
     * @param selector Existing queue UUID or exact user-facing name, borrowed only for this call.
     * @return Success after pending work is cancelled, current secret references are removed, and the active name is
     * released. Not-found, not-suspended, running-work, revision-exhausted, state, invariant, and database failures
     * are returned as Error values. Definitions and terminal history remain durable for retention.
     */
    [[nodiscard]] auto delete_queue(QueueSelector const& selector) -> jb::core::Result<void, jb::core::Error>;

    /** Creates one active job and its first schedule-owned run in one immediate transaction.
     * @param request Queue selector, definition fields, partial attributes, owning payload, and optional idempotency
     * key consumed after validation. A matching key in the resolved queue scope replays the original successful
     * result and its unchanged first occurrence. A fresh cron request is validated and evaluated strictly after the
     * transaction's sampled current time.
     * @return Committed or replayed revision-1 definition, or a queue, schedule, validation, idempotency, generator,
     * attribute, run, or database Error. No attempt is created and no runner execution starts.
     */
    [[nodiscard]] auto create_job(CreateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Updates one job definition and, when unstarted, its scheduled-run snapshot in one immediate transaction.
     * @param request Non-empty patch consumed after validation. expected_revision must match the durable positive
     * revision. Updates preserve stored attributes not named in attribute_changes and never reapply defaults. A
     * replacement cron schedule is validated synchronously without retaining its strings.
     * @return Committed definition with its revision incremented once, or a validation, not-found, deleted, revision,
     * state, immutable, schedule-snapshot, cron, attribute, payload, or database Error. An unstarted occurrence keeps
     * its run identity and receives the new revision, snapshot, and schedule time atomically. A running or
     * retry-waiting recurring occurrence keeps its old snapshot; supplied definition fields update only the durable
     * definition and apply to its future successor. Conversion to a one-time schedule and snapshot refresh require the
     * current occurrence to be scheduled with no attempt; even a pending attempt prevents them. One-time definitions
     * remain immutable after execution starts. No attempt, callback, or external work starts.
     */
    [[nodiscard]] auto update_job(UpdateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Creates one manual scheduled run while preserving the job's schedule-owned occurrence.
     * @param request Job ID and optional job-scoped idempotency key consumed after validation. A matching key replays
     * the original committed manual run without allocating another UUID.
     * @return Committed or replayed manual run, or a job, queue, manual-precondition, idempotency, generator, storage,
     * or database Error. A fresh operation requires one future scheduled schedule-owned occurrence, no running or
     * retry-waiting run, and no other non-terminal manual run. Job suspension is permitted; queue suspension prevents
     * later scheduler dispatch but not creation. The returned run snapshots the current definition and sets both
     * planned_at and runnable_at to the transaction's single sampled current time. No callback or execution starts.
     */
    [[nodiscard]] auto run_now(RunNowRequest request) -> jb::core::Result<JobRun, jb::core::Error>;

    /** Suspends one job without changing its schedule-owned run or execution snapshot.
     * @param id Stable job-definition UUID, borrowed only for this call.
     * @return Committed definition. Active jobs pass through suspending and complete immediately when no running work
     * is present; suspending jobs complete when running work has drained. Each actual transition increments the
     * revision once, while suspended jobs are returned unchanged. Deleted, not-found, state-conflict,
     * revision-exhausted, and database failures are reported as Error values.
     */
    [[nodiscard]] auto suspend_job(jb::core::Uuid const& id) -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Resumes one suspending or suspended job without changing its schedule-owned run or execution snapshot.
     * @param id Stable job-definition UUID, borrowed only for this call.
     * @return Committed active definition with one revision increment, the unchanged definition when already active,
     * or a deleted, not-found, state-conflict, revision-exhausted, or database Error.
     */
    [[nodiscard]] auto resume_job(jb::core::Uuid const& id) -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Moves one fully suspended job and its non-terminal execution snapshots in one immediate transaction.
     * @param request Job ID, positive expected revision, and distinct active or suspended destination queue.
     * @return Committed suspended definition with one revision increment, or a validation, not-found, deleted,
     * not-suspended, revision, queue-state, invariant, or database Error. Complete definition fields are preserved,
     * destination defaults are not applied, and terminal run queue IDs are unchanged.
     */
    [[nodiscard]] auto move_job(MoveJobRequest const& request) -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Soft-deletes one fully suspended job in one immediate transaction.
     * @param request Job ID and positive expected revision.
     * @return Success after one revision increment, pending-run cancellation, and current secret-reference cleanup,
     * or a validation, not-found, deleted, not-suspended, revision, running-work, invariant, or database Error.
     * Definition and execution history remain durable for retention.
     */
    [[nodiscard]] auto delete_job(DeleteJobRequest const& request) -> jb::core::Result<void, jb::core::Error>;

    /** Gets one job definition by stable ID without starting an explicit transaction.
     * @param id Stable job-definition UUID.
     * @param include_deleted Permits lookup of a soft-deleted definition.
     * @return Job definition, `jobu.job.not_found`, or a storage/database Error.
     */
    [[nodiscard]] auto get_job(jb::core::Uuid const& id, bool include_deleted = false)
        -> jb::core::Result<JobDefinition, jb::core::Error>;

    /** Lists a bounded page using ascending UUID-byte keyset order.
     * @param request Optional queue/state/type/deletion filters and a limit from 1 through 200. A queue name is
     * resolved exactly before listing; deleted queues require include_deleted.
     * @return At most the requested number of definitions and a cursor only when another row exists, or an Error.
     */
    [[nodiscard]] auto list_jobs(JobListRequest const& request) -> jb::core::Result<JobPage, jb::core::Error>;

private:
    struct Private;
};

} // namespace jb::jobu
