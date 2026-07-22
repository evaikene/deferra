#include "retention_repository_priv.hpp"

#include "query.hpp"
#include "transaction.hpp"

#include <cstdint>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRetentionBatch = 1000;

auto invalid_limit() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.storage.invalid_limit",
        .message  = "Repository limit is outside its supported range",
    };
}

auto invalid_count(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "jobu.storage.invariant",
        .message  = "Persisted retention data violates a JobU invariant",
        .detail   = "reason=" + std::string{reason},
    };
}

auto affected_rows(jb::db::Query const& query) -> RepositoryResult<std::size_t>
{
    auto const count = query.num_rows_affected();
    if (count < 0 || !std::in_range<std::size_t>(count)) {
        return RepositoryResult<std::size_t>::failure(invalid_count("invalid_affected_row_count"));
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(count));
}

auto purge_jobs(jb::db::Database& database, std::size_t limit) -> RepositoryResult<std::size_t>
{
    jb::db::Query query{database};
    auto          prepared = query.prepare(
        "DELETE FROM jobu_jobs WHERE state = 'deleted' "
        "AND NOT EXISTS (SELECT 1 FROM jobu_runs WHERE jobu_runs.job_id = jobu_jobs.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_idempotency WHERE jobu_idempotency.resource_id = jobu_jobs.id) "
        "AND id IN (SELECT candidates.id FROM jobu_jobs AS candidates WHERE candidates.state = 'deleted' "
        "AND NOT EXISTS (SELECT 1 FROM jobu_runs WHERE jobu_runs.job_id = candidates.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_idempotency WHERE jobu_idempotency.resource_id = candidates.id) "
        "ORDER BY candidates.deleted_at_us ASC, candidates.id ASC LIMIT :limit)");
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

auto purge_queues(jb::db::Database& database, std::size_t limit) -> RepositoryResult<std::size_t>
{
    jb::db::Query query{database};
    auto          prepared = query.prepare(
        "DELETE FROM jobu_queues WHERE state = 'deleted' "
        "AND NOT EXISTS (SELECT 1 FROM jobu_jobs WHERE jobu_jobs.queue_id = jobu_queues.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_runs WHERE jobu_runs.queue_id = jobu_queues.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_idempotency WHERE jobu_idempotency.resource_id = jobu_queues.id "
        "OR jobu_idempotency.scope_id = jobu_queues.id) "
        "AND id IN (SELECT candidates.id FROM jobu_queues AS candidates WHERE candidates.state = 'deleted' "
        "AND NOT EXISTS (SELECT 1 FROM jobu_jobs WHERE jobu_jobs.queue_id = candidates.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_runs WHERE jobu_runs.queue_id = candidates.id) "
        "AND NOT EXISTS (SELECT 1 FROM jobu_idempotency WHERE jobu_idempotency.resource_id = candidates.id "
        "OR jobu_idempotency.scope_id = candidates.id) "
        "ORDER BY candidates.deleted_at_us ASC, candidates.id ASC LIMIT :limit)");
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

} // anonymous namespace

RetentionRepository::RetentionRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept
    : _database{database}
    , _runs{database, attributes}
    , _idempotency{database}
{}

auto RetentionRepository::purge_batch(jb::core::UtcTimePoint cutoff, std::size_t limit)
    -> jb::core::Result<RetentionPurgeCounts, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumRetentionBatch || !std::in_range<std::int64_t>(limit)) {
        return RepositoryResult<RetentionPurgeCounts>::failure(invalid_limit());
    }
    auto begun = jb::db::Transaction::begin(_database);
    if (!begun) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto run_ids     = _runs.list_terminal_before(cutoff, limit);
    if (!run_ids) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(run_ids).error());
    }
    auto deleted_runs = _runs.delete_selected_terminal(*run_ids);
    if (!deleted_runs) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(deleted_runs).error());
    }
    if (*deleted_runs != run_ids->size()) {
        return RepositoryResult<RetentionPurgeCounts>::failure(invalid_count("terminal_run_delete_count"));
    }
    auto deleted_idempotency = _idempotency.erase_expired(cutoff, limit);
    if (!deleted_idempotency) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(deleted_idempotency).error());
    }
    auto deleted_jobs = purge_jobs(_database, limit);
    if (!deleted_jobs) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(deleted_jobs).error());
    }
    auto deleted_queues = purge_queues(_database, limit);
    if (!deleted_queues) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(deleted_queues).error());
    }
    auto counts = RetentionPurgeCounts{
        .runs                = *deleted_runs,
        .idempotency_records = *deleted_idempotency,
        .jobs                = *deleted_jobs,
        .queues              = *deleted_queues,
    };
    auto committed = transaction.commit();
    if (!committed) {
        return RepositoryResult<RetentionPurgeCounts>::failure(std::move(committed).error());
    }
    return RepositoryResult<RetentionPurgeCounts>::success(counts);
}

} // namespace jb::jobu::detail
