#include "retention_repository_priv.hpp"

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

auto invalid_count() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.retention.invalid_relationship",
            .message  = "Persisted retention data violates a JobU invariant",
            .detail   = "reason=terminal_run_delete_count"};
}

auto purge_queue_history(RunRepository&              runs,
                         QueueRetentionPolicy const& policy,
                         jb::core::UtcTimePoint      sweep_now,
                         std::chrono::seconds        daemon_retention,
                         std::size_t                 limit) -> RepositoryResult<std::size_t>
{
    auto const retention = policy.retention.value_or(daemon_retention);
    if (retention == std::chrono::seconds::zero()) {
        return RepositoryResult<std::size_t>::success(0);
    }
    auto const cutoff = retention_cutoff(sweep_now, retention);

    // Run pages are local to this visit, not an unbounded per-queue cursor map.
    // The caller's one transaction makes all pages and their cascades atomic.
    auto deleted = std::size_t{0};
    auto after   = std::optional<TerminalRunKey>{};
    while (deleted < limit) {
        auto const page_limit = std::min(kRunPageSize, limit - deleted);
        auto       page       = runs.list_terminal_before(policy.id, cutoff, page_limit, after);
        if (!page) {
            return RepositoryResult<std::size_t>::failure(std::move(page).error());
        }
        if (page->empty()) {
            break;
        }
        auto ids = std::vector<jb::core::Uuid>{};
        ids.reserve(page->size());
        for (auto const& key : *page) {
            ids.push_back(key.id);
        }
        auto count = runs.delete_selected_terminal(policy.id, cutoff, ids);
        if (!count) {
            return RepositoryResult<std::size_t>::failure(std::move(count).error());
        }
        if (*count != page->size()) {
            return RepositoryResult<std::size_t>::failure(invalid_count());
        }
        deleted += *count;
        after    = page->back();
        if (page->size() < page_limit) {
            break;
        }
    }
    return RepositoryResult<std::size_t>::success(deleted);
}

auto purge_visit(jb::db::Database&           database,
                 QueueRepository&            queues,
                 RunRepository&              runs,
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

    // Policy, historical queue ownership and deletion all share this snapshot.
    auto policy = queues.next_retention_policy(cursor.after_queue);
    if (!policy) {
        return RepositoryResult<RetentionBatchResult>::failure(std::move(policy).error());
    }
    auto result = RetentionBatchResult{};
    if (*policy) {
        auto deleted = purge_queue_history(runs, **policy, sweep_now, daemon_retention, limit);
        if (!deleted) {
            return RepositoryResult<RetentionBatchResult>::failure(std::move(deleted).error());
        }
        result.purged.runs      = *deleted;
        result.next.after_queue = (**policy).id;
    }
    else {
        // Stage 9.7 will append owner/key phases before this reset boundary.
        result.sweep_complete = true;
    }

    auto committed = transaction.commit();
    if (!committed) {
        return RepositoryResult<RetentionBatchResult>::failure(std::move(committed).error());
    }
    return RepositoryResult<RetentionBatchResult>::success(result);
}

} // anonymous namespace

RetentionRepository::RetentionRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _runs{database, attributes}
    , _queues{database, attributes}
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

    auto result = purge_visit(_database, _queues, _runs, sweep_now, daemon_retention, limit, cursor);
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
