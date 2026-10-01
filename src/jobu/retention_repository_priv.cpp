#include "retention_repository_priv.hpp"

#include "idempotency_codec_priv.hpp"
#include "storage_failure_priv.hpp"
#include "transaction.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRetentionBatch = 1000;
constexpr std::size_t kRunPageSize           = 500;

// This calculation never converts a potentially huge seconds duration to native
// clock ticks. Saturation means no representable durable completion is old enough.
auto retention_cutoff(jb::core::UtcTimePoint now, std::chrono::seconds retention) -> jb::core::UtcTimePoint
{
    using Microseconds = std::chrono::microseconds;
    auto const minimum = std::chrono::ceil<Microseconds>(jb::core::UtcTimePoint::min().time_since_epoch()).count();
    auto       tick    = std::chrono::ceil<Microseconds>(now.time_since_epoch()).count();

    // Unsigned subtraction gives the exact nonnegative distance even when the
    // signed endpoints straddle zero. Check before multiplying seconds to micros.
    auto const     available         = static_cast<std::uint64_t>(tick) - static_cast<std::uint64_t>(minimum);
    constexpr auto micros_per_second = std::uint64_t{1'000'000};
    auto const     seconds           = static_cast<std::uint64_t>(retention.count());
    if (seconds > available / micros_per_second) {
        tick = minimum;
    }
    else {
        auto remaining = seconds * micros_per_second;
        // At most three signed subtractions cover the entire uint64 range. Each
        // intermediate stays within the checked durable range, without narrowing.
        while (remaining != 0) {
            auto const step = std::min(remaining, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
            tick      -= static_cast<std::int64_t>(step);
            remaining -= step;
        }
    }
    return jb::core::UtcTimePoint{std::chrono::duration_cast<jb::core::UtcClock::duration>(Microseconds{tick})};
}

auto invalid_options(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.retention.invalid_options",
            .message  = "Retention options are outside their supported range",
            .detail   = "reason=" + std::string{reason}};
}

auto invalid_relationship(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.retention.invalid_relationship",
            .message  = "Persisted retention data violates a JobU invariant",
            .detail   = "reason=" + std::string{reason}};
}

struct ReferenceSummary {
    std::size_t retiring{0};
    bool        pinned{false};
};

// Replay documents are immutable snapshots. Validate identity and original scope,
// never mutable definition fields against their current values after an update/move.
class ReplayRetirement final {
public:
    ReplayRetirement(IdempotencyRepository&    records,
                     RetentionOwnerRepository& owners,
                     AttributeRegistry const&  attributes) noexcept
        : _records{records}
        , _owners{owners}
        , _attributes{attributes}
    {}

    auto inspect(jb::core::Uuid const& owner, std::string_view retiring_method) -> RepositoryResult<ReferenceSummary>
    {
        auto summary = ReferenceSummary{};
        auto after   = std::optional<IdempotencyKey>{};
        for (;;) {
            auto page = _records.list_referencing(owner, kReferencePageSize, after);
            if (!page) {
                return RepositoryResult<ReferenceSummary>::failure(reference_error(std::move(page).error()));
            }
            if (page->empty()) {
                break;
            }
            for (auto const& record : *page) {
                auto valid = validate(record);
                if (!valid) {
                    return RepositoryResult<ReferenceSummary>::failure(std::move(valid).error());
                }
                if (record.method == retiring_method && record.resource_id == owner) {
                    ++summary.retiring;
                }
                else {
                    summary.pinned = true;
                }
            }
            auto const& last = page->back();
            after            = IdempotencyKey{.method = last.method, .scope_id = last.scope_id, .key = last.key};
        }
        return RepositoryResult<ReferenceSummary>::success(summary);
    }

    auto erase(jb::core::Uuid const& owner, std::string_view method, std::size_t expected)
        -> RepositoryResult<std::size_t>
    {
        // The preflight above validates all references before any delete. A second
        // bounded traversal avoids retaining every document in memory. Both passes
        // share the caller's transaction, including the eventual resource deletion.
        auto count = std::size_t{0};
        auto after = std::optional<IdempotencyKey>{};
        for (;;) {
            auto page = _records.list_referencing(owner, kReferencePageSize, after);
            if (!page) {
                return RepositoryResult<std::size_t>::failure(reference_error(std::move(page).error()));
            }
            if (page->empty()) {
                break;
            }
            auto const& last = page->back();
            after            = IdempotencyKey{.method = last.method, .scope_id = last.scope_id, .key = last.key};
            for (auto const& record : *page) {
                if (record.method != method || record.resource_id != owner) {
                    continue;
                }
                auto erased = _records.erase_matching(record);
                if (!erased) {
                    return RepositoryResult<std::size_t>::failure(std::move(erased).error());
                }
                if (*erased != 1) {
                    return RepositoryResult<std::size_t>::failure(invalid_relationship("replay_delete_count"));
                }
                ++count;
            }
        }
        if (count != expected) {
            return RepositoryResult<std::size_t>::failure(invalid_relationship("replay_set_changed"));
        }
        return RepositoryResult<std::size_t>::success(count);
    }

private:
    static constexpr std::size_t kReferencePageSize = 16;

    static auto reference_error(jb::core::Error error) -> jb::core::Error
    {
        if (error.code == "jobu.idempotency.invalid_record" || error.code.starts_with("jobu.storage.invalid_")) {
            return invalid_relationship("replay_record");
        }
        return error;
    }

    auto validate(IdempotencyRecord const& record) -> RepositoryResult<void>
    {
        if (record.method == "queue.create") {
            auto request = validate_queue_create_idempotency_request(record.request_json, _attributes);
            auto replay  = decode_queue_idempotency_result(record.result_json, _attributes);
            if (!request || !replay || record.scope_id != jb::core::Uuid{} || replay->id != record.resource_id) {
                return RepositoryResult<void>::failure(invalid_relationship("queue_creation_scope"));
            }
            auto owner = _owners.has_queue(record.resource_id);
            if (!owner) {
                return RepositoryResult<void>::failure(std::move(owner).error());
            }
            if (!*owner) {
                return RepositoryResult<void>::failure(invalid_relationship("queue_creation_owner"));
            }
        }
        else if (record.method == "job.create") {
            auto scope  = decode_job_create_idempotency_scope(record.request_json, _attributes);
            auto replay = decode_job_idempotency_result(record.result_json, _attributes);
            if (!scope || !replay || *scope != record.scope_id || replay->queue_id != record.scope_id ||
                replay->id != record.resource_id) {
                return RepositoryResult<void>::failure(invalid_relationship("job_creation_scope"));
            }
            auto job   = _owners.has_job(record.resource_id);
            auto queue = _owners.has_queue(record.scope_id);
            if (!job) {
                return RepositoryResult<void>::failure(std::move(job).error());
            }
            if (!queue) {
                return RepositoryResult<void>::failure(std::move(queue).error());
            }
            if (!*job || !*queue) {
                return RepositoryResult<void>::failure(invalid_relationship("job_creation_owner"));
            }
        }
        else if (record.method == "job.run_now") {
            auto scope  = decode_run_now_idempotency_scope(record.request_json);
            auto replay = decode_run_now_idempotency_result(record.result_json, _attributes);
            if (!scope || !replay || *scope != record.scope_id || replay->job_id != record.scope_id ||
                replay->id != record.resource_id) {
                return RepositoryResult<void>::failure(invalid_relationship("manual_replay_scope"));
            }
            auto run = _owners.find_run(record.resource_id);
            auto job = _owners.has_job(record.scope_id);
            if (!run) {
                return RepositoryResult<void>::failure(std::move(run).error());
            }
            if (!job) {
                return RepositoryResult<void>::failure(std::move(job).error());
            }
            if (!*run || !*job || !(**run).manual || (**run).job_id != record.scope_id) {
                return RepositoryResult<void>::failure(invalid_relationship("manual_replay_owner"));
            }
        }
        else {
            return RepositoryResult<void>::failure(invalid_relationship("unknown_replay_method"));
        }
        return RepositoryResult<void>::success();
    }

    IdempotencyRepository&    _records;
    RetentionOwnerRepository& _owners;
    AttributeRegistry const&  _attributes;
};

auto purge_queue_history(RunRepository&              runs,
                         ReplayRetirement&           replays,
                         QueueRetentionPolicy const& policy,
                         jb::core::UtcTimePoint      sweep_now,
                         std::chrono::seconds        daemon_retention,
                         std::size_t                 limit) -> RepositoryResult<RetentionPurgeCounts>
{
    auto       result    = RetentionPurgeCounts{};
    auto const retention = policy.retention.value_or(daemon_retention);
    if (retention == std::chrono::seconds::zero()) {
        return RepositoryResult<RetentionPurgeCounts>::success(result);
    }
    auto const cutoff = retention_cutoff(sweep_now, retention);

    // A run and its replay are one lifetime unit. Check and retire the replay
    // while the run still exists; any later failure rolls both deletions back.
    auto after = std::optional<TerminalRunKey>{};
    while (result.runs < limit) {
        auto const page_limit = std::min(kRunPageSize, limit - result.runs);
        auto       page       = runs.list_terminal_before(policy.id, cutoff, page_limit, after);
        if (!page) {
            return RepositoryResult<RetentionPurgeCounts>::failure(std::move(page).error());
        }
        if (page->empty()) {
            break;
        }
        auto ids = std::vector<jb::core::Uuid>{};
        ids.reserve(page->size());
        for (auto const& key : *page) {
            auto references = replays.inspect(key.id, "job.run_now");
            if (!references) {
                return RepositoryResult<RetentionPurgeCounts>::failure(std::move(references).error());
            }
            if (references->pinned) {
                return RepositoryResult<RetentionPurgeCounts>::failure(invalid_relationship("run_replay_pin"));
            }
            if (references->retiring != 0) {
                auto retired = replays.erase(key.id, "job.run_now", references->retiring);
                if (!retired) {
                    return RepositoryResult<RetentionPurgeCounts>::failure(std::move(retired).error());
                }
                result.idempotency_records += *retired;
            }
            ids.push_back(key.id);
        }
        auto count = runs.delete_selected_terminal(policy.id, cutoff, ids);
        if (!count) {
            return RepositoryResult<RetentionPurgeCounts>::failure(std::move(count).error());
        }
        if (*count != page->size()) {
            return RepositoryResult<RetentionPurgeCounts>::failure(invalid_relationship("terminal_run_delete_count"));
        }
        result.runs += *count;
        after        = page->back();
        if (page->size() < page_limit) {
            break;
        }
    }
    return RepositoryResult<RetentionPurgeCounts>::success(result);
}

auto purge_owner_page(RetentionOwnerRepository&   owners,
                      ReplayRetirement&           replays,
                      RetentionOwnerKind          kind,
                      std::size_t                 limit,
                      RetentionSweepCursor const& cursor) -> RepositoryResult<RetentionBatchResult>
{
    auto page = owners.list_candidates(kind, limit, cursor.after_owner);
    if (!page) {
        return RepositoryResult<RetentionBatchResult>::failure(std::move(page).error());
    }
    auto result = RetentionBatchResult{.next = cursor};
    for (auto const& id : *page) {
        if (kind == RetentionOwnerKind::DeletedJob) {
            // Soft deletion already removes current references. Do not let the
            // physical-delete cascade conceal a broken secret ownership invariant.
            auto references = owners.has_secret_references(id);
            if (!references) {
                return RepositoryResult<RetentionBatchResult>::failure(std::move(references).error());
            }
            if (*references) {
                return RepositoryResult<RetentionBatchResult>::failure(invalid_relationship("deleted_job_secret_refs"));
            }
        }
        auto const* method     = kind == RetentionOwnerKind::DeletedQueue ? "queue.create" : "job.create";
        auto        references = replays.inspect(id, method);
        if (!references) {
            return RepositoryResult<RetentionBatchResult>::failure(std::move(references).error());
        }
        if (kind != RetentionOwnerKind::FinishedOnce && references->pinned) {
            continue;
        }
        if (references->retiring != 0) {
            auto erased = replays.erase(id, method, references->retiring);
            if (!erased) {
                return RepositoryResult<RetentionBatchResult>::failure(std::move(erased).error());
            }
            result.purged.idempotency_records += *erased;
        }
        if (kind != RetentionOwnerKind::FinishedOnce) {
            auto deleted = owners.delete_owner(kind, id);
            if (!deleted) {
                return RepositoryResult<RetentionBatchResult>::failure(std::move(deleted).error());
            }
            if (*deleted != 1) {
                return RepositoryResult<RetentionBatchResult>::failure(invalid_relationship("owner_delete_count"));
            }
            if (kind == RetentionOwnerKind::DeletedJob) {
                ++result.purged.jobs;
            }
            else {
                ++result.purged.queues;
            }
        }
    }

    if (page->size() == limit) {
        result.next.after_owner = page->back();
    }
    else {
        result.next.after_owner.reset();
        switch (cursor.phase) {
            case RetentionSweepPhase::OnceKeys:
                result.next.phase = RetentionSweepPhase::DeletedJobs;
                break;
            case RetentionSweepPhase::DeletedJobs:
                result.next.phase = RetentionSweepPhase::DeletedQueues;
                break;
            case RetentionSweepPhase::DeletedQueues:
                result.next           = {};
                result.sweep_complete = true;
                break;
            case RetentionSweepPhase::History:
                return RepositoryResult<RetentionBatchResult>::failure(invalid_options("owner_phase"));
        }
    }
    return RepositoryResult<RetentionBatchResult>::success(result);
}

auto purge_visit(jb::db::Database&           database,
                 QueueRepository&            queues,
                 RunRepository&              runs,
                 IdempotencyRepository&      records,
                 RetentionOwnerRepository&   owners,
                 AttributeRegistry const&    attributes,
                 jb::core::UtcTimePoint      sweep_now,
                 std::chrono::seconds        daemon_retention,
                 std::size_t                 limit,
                 RetentionSweepCursor const& cursor) -> RepositoryResult<RetentionBatchResult>
{
    auto begun = jb::db::Transaction::begin(database);
    if (!begun) {
        return RepositoryResult<RetentionBatchResult>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto replays     = ReplayRetirement{records, owners, attributes};
    auto result      = RepositoryResult<RetentionBatchResult>::success({.next = cursor});

    // Policy, ownership, replay retirement and resource deletion share one snapshot.
    switch (cursor.phase) {
        case RetentionSweepPhase::History: {
            auto policy = queues.next_retention_policy(cursor.after_queue);
            if (!policy) {
                return RepositoryResult<RetentionBatchResult>::failure(std::move(policy).error());
            }
            if (*policy) {
                auto deleted = purge_queue_history(runs, replays, **policy, sweep_now, daemon_retention, limit);
                if (!deleted) {
                    return RepositoryResult<RetentionBatchResult>::failure(std::move(deleted).error());
                }
                result->purged           = *deleted;
                result->next.after_queue = (**policy).id;
            }
            else {
                result->next = {.phase = RetentionSweepPhase::OnceKeys};
            }
            break;
        }
        case RetentionSweepPhase::OnceKeys:
            result = purge_owner_page(owners, replays, RetentionOwnerKind::FinishedOnce, limit, cursor);
            break;
        case RetentionSweepPhase::DeletedJobs:
            result = purge_owner_page(owners, replays, RetentionOwnerKind::DeletedJob, limit, cursor);
            break;
        case RetentionSweepPhase::DeletedQueues:
            result = purge_owner_page(owners, replays, RetentionOwnerKind::DeletedQueue, limit, cursor);
            break;
    }
    if (!result) {
        return result;
    }
    auto committed = transaction.commit();
    if (!committed) {
        return RepositoryResult<RetentionBatchResult>::failure(std::move(committed).error());
    }
    return result;
}

} // anonymous namespace

RetentionRepository::RetentionRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _runs{database, attributes}
    , _queues{database, attributes}
    , _idempotency{database}
    , _owners{database}
    , _attributes{attributes}
{}

auto RetentionRepository::purge_next_batch(jb::core::UtcTimePoint      sweep_now,
                                           std::chrono::seconds        daemon_retention,
                                           std::size_t                 limit,
                                           RetentionSweepCursor const& cursor)
    -> jb::core::Result<RetentionBatchResult, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumRetentionBatch) {
        return RepositoryResult<RetentionBatchResult>::failure(invalid_options("batch_size"));
    }
    if (daemon_retention < std::chrono::seconds::zero()) {
        return RepositoryResult<RetentionBatchResult>::failure(invalid_options("negative_retention"));
    }

    if ((cursor.phase == RetentionSweepPhase::History && cursor.after_owner) ||
        (cursor.phase != RetentionSweepPhase::History && cursor.after_queue) ||
        (cursor.phase != RetentionSweepPhase::History && cursor.phase != RetentionSweepPhase::OnceKeys &&
         cursor.phase != RetentionSweepPhase::DeletedJobs && cursor.phase != RetentionSweepPhase::DeletedQueues)) {
        return RepositoryResult<RetentionBatchResult>::failure(invalid_options("cursor"));
    }

    auto result = purge_visit(_database,
                              _queues,
                              _runs,
                              _idempotency,
                              _owners,
                              _attributes,
                              sweep_now,
                              daemon_retention,
                              limit,
                              cursor);
    if (!result) {
        // The transaction guard has already unwound, including any rollback.
        // Preserve driver codes, but never return SQL/backend detail to the future service.
        auto const origin = result.error().code == "jobu.retention.invalid_relationship"
                              ? StorageFailureOrigin::PersistedData
                              : StorageFailureOrigin::Operation;
        return RepositoryResult<RetentionBatchResult>::failure(
            sanitized_storage_error(result.error(), StorageOperation::Mutation, origin));
    }
    return result;
}

} // namespace jb::jobu::detail
