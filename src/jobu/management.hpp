/** @file management.hpp
 * @brief Defines the synchronous JobU management service and queue/job requests.
 */
#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "job.hpp"
#include "queue.hpp"
#include "result.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu {

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

/** Values used to create one active one-time job and its scheduled run.
 *
 * Phase 3 accepts only OnceSchedule. The supplied job attribute layer is materialized over built-in, daemon, and
 * resolved queue defaults before persistence. The payload remains owning and preserves unknown additive members.
 */
struct CreateJobRequest {
    /// Existing non-deleted queue selected by UUID or exact user-facing name.
    QueueSelector              queue;
    /// Optional non-unique display name containing at most 256 valid UTF-8 bytes and no ASCII controls.
    std::optional<std::string> name;
    /// Runner family whose structural payload rules are applied.
    JobType                    type{JobType::Cli};
    /// One-time schedule accepted by Phase 3; CronSchedule returns `jobu.schedule.cron_unavailable`.
    JobSchedule                schedule;
    /// Signed scheduling priority copied into the run snapshot.
    std::int32_t               priority{0};
    /// Partial job-specific attribute layer.
    AttributeSet               attributes;
    /// Owning CLI or HTTP payload object, limited to 256 KiB when deterministically encoded.
    jb::rpc::JsonValue         payload;
    /// Optional 1-through-128-byte UTF-8 key reserved for durable create replay.
    std::optional<std::string> idempotency_key;
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

/** Synchronous owner-thread service for durable JobU management operations.
 *
 * The service borrows an already-open Database, AttributeRegistry, UuidGenerator, and TimeSource; each collaborator
 * must outlive the service. All calls must run on the Database owner thread. The daemon-default attribute layer is
 * validated and copied during construction; a validation failure is returned by subsequent operations.
 *
 * Queue/job gets and lists use bounded repository reads without an explicit transaction. Creates and queue updates use
 * one immediate transaction and return only after commit. Errors include stable `jobu.queue.*`, `jobu.job.*`,
 * `jobu.attribute.*`, and `jobu.idempotency.*` codes plus unchanged database errors when no domain mapping applies.
 * Methods invoke no callbacks, start no threads, and perform no event-loop processing or external I/O.
 */
class ManagementService final {
public:
    /** Constructs a management service by borrowing its collaborators.
     * @param database Already-open sole JobU database connection.
     * @param attributes Registry used to validate and decode durable attributes.
     * @param uuid_generator Generator used for new durable identities.
     * @param time_source Source used for durable UTC timestamps.
     * @param daemon_defaults Partial daemon-default attribute layer to validate and copy.
     */
    ManagementService(jb::db::Database&        database,
                      AttributeRegistry const& attributes,
                      jb::core::UuidGenerator& uuid_generator,
                      jb::core::TimeSource&    time_source,
                      AttributeSet             daemon_defaults = {});

    /// Destroys private repositories without changing the borrowed Database state.
    ~ManagementService();

    /// Prevents copying borrowed collaborators and repository state.
    ManagementService(ManagementService const&)                    = delete;
    /// Prevents moving a service whose repositories borrow a fixed Database.
    ManagementService(ManagementService&&)                         = delete;
    /// Prevents copy assignment of borrowed collaborators and repository state.
    auto operator=(ManagementService const&) -> ManagementService& = delete;
    /// Prevents move assignment of repositories that borrow a fixed Database.
    auto operator=(ManagementService&&) -> ManagementService&      = delete;

    /** Creates one active queue in an immediate transaction.
     * @param request Queue configuration consumed after validation; the optional idempotency key is syntax-checked
     * but durable replay semantics are introduced with the idempotency repository.
     * @return Committed queue, or a validation, name-conflict, generator, attribute, or database Error.
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

    /** Creates one active one-time job and its scheduled run in one immediate transaction.
     * @param request Queue selector, definition fields, partial attributes, owning payload, and optional idempotency
     * key consumed after validation. The key is syntax-checked; durable replay semantics are introduced with the
     * idempotency repository.
     * @return Committed revision-1 definition, or a queue, schedule, validation, generator, attribute, run, or database
     * Error. No attempt is created and no external work starts.
     */
    [[nodiscard]] auto create_job(CreateJobRequest request) -> jb::core::Result<JobDefinition, jb::core::Error>;

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
    std::unique_ptr<Private> _data;
};

} // namespace jb::jobu
