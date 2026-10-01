#include "retention_owner_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "run.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr auto kNoJobRuns     = "NOT EXISTS (SELECT 1 FROM jobu_runs WHERE job_id = jobu_jobs.id)";
constexpr auto kNoQueueOwners = "NOT EXISTS (SELECT 1 FROM jobu_jobs WHERE queue_id = jobu_queues.id) "
                                "AND NOT EXISTS (SELECT 1 FROM jobu_runs WHERE queue_id = jobu_queues.id)";

auto invalid_relationship() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.retention.invalid_relationship",
            .message  = "Persisted retention data violates a JobU invariant"};
}

auto prepare_owner_query(jb::db::Query& query, std::string_view sql, jb::core::Uuid const& id) -> RepositoryResult<void>
{
    auto result = query.prepare(sql);
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(id));
    }
    if (result) {
        result = query.exec();
    }
    return result;
}

auto has_row(jb::db::Database& database, std::string_view sql, jb::core::Uuid const& id) -> RepositoryResult<bool>
{
    jb::db::Query query{database};
    auto          result = prepare_owner_query(query, sql, id);
    if (!result) {
        return RepositoryResult<bool>::failure(std::move(result).error());
    }
    auto next = query.next();
    if (!next) {
        return next;
    }
    auto finished = query.finish();
    if (!finished) {
        return RepositoryResult<bool>::failure(std::move(finished).error());
    }
    return next;
}

} // anonymous namespace

RetentionOwnerRepository::RetentionOwnerRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto RetentionOwnerRepository::list_candidates(RetentionOwnerKind            kind,
                                               std::size_t                   limit,
                                               std::optional<jb::core::Uuid> after)
    -> jb::core::Result<std::vector<jb::core::Uuid>, jb::core::Error>
{
    using PageResult = RepositoryResult<std::vector<jb::core::Uuid>>;
    if (limit == 0 || limit > 1000) {
        return PageResult::failure({.category = jb::core::ErrorCategory::InvalidArgument,
                                    .code     = "jobu.storage.invalid_limit",
                                    .message  = "Repository limit is outside its supported range"});
    }

    // Keep tombstones until all domain children are gone. Replay pins are checked
    // separately so eligible creation keys can be retired with their owner.
    auto sql = std::string{};
    switch (kind) {
        case RetentionOwnerKind::FinishedOnce:
            sql  = "SELECT id AS owner_id FROM jobu_jobs WHERE schedule_kind = 'once' "
                   "AND state IN ('succeeded', 'failed', 'cancelled') AND ";
            sql += kNoJobRuns;
            break;
        case RetentionOwnerKind::DeletedJob:
            sql  = "SELECT id AS owner_id FROM jobu_jobs WHERE state = 'deleted' AND ";
            sql += kNoJobRuns;
            break;
        case RetentionOwnerKind::DeletedQueue:
            sql  = "SELECT id AS owner_id FROM jobu_queues WHERE state = 'deleted' AND ";
            sql += kNoQueueOwners;
            break;
    }
    // An explicit first-page lower bound also selects the UUID index instead
    // of sorting every candidate owner through a different covering index.
    // Nil is included on the first page; continuation excludes its last UUID.
    sql += after ? " AND id > :after" : " AND id >= :after";
    sql += " ORDER BY id LIMIT :limit";

    jb::db::Query query{_database};
    auto          result = query.prepare(sql);
    if (result) {
        result = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    }
    if (result) {
        result = query.bind_value(":after", uuid_to_storage(after.value_or(jb::core::Uuid{})));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return PageResult::failure(std::move(result).error());
    }

    auto ids = std::vector<jb::core::Uuid>{};
    ids.reserve(limit);
    for (;;) {
        auto next = query.next();
        if (!next) {
            return PageResult::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto id = read_uuid(query.record(), "owner_id");
        if (!id) {
            return PageResult::failure(std::move(id).error());
        }
        ids.push_back(*id);
    }
    auto finished = query.finish();
    if (!finished) {
        return PageResult::failure(std::move(finished).error());
    }
    return PageResult::success(std::move(ids));
}

auto RetentionOwnerRepository::has_job(jb::core::Uuid const& id) -> jb::core::Result<bool, jb::core::Error>
{
    return has_row(_database, "SELECT 1 FROM jobu_jobs WHERE id = :id", id);
}

auto RetentionOwnerRepository::find_run(jb::core::Uuid const& id)
    -> jb::core::Result<std::optional<RetentionRunOwner>, jb::core::Error>
{
    using FindResult = RepositoryResult<std::optional<RetentionRunOwner>>;
    jb::db::Query query{_database};
    auto result = prepare_owner_query(query, "SELECT job_id, origin, schedule_owned FROM jobu_runs WHERE id = :id", id);
    if (!result) {
        return FindResult::failure(std::move(result).error());
    }
    auto next = query.next();
    if (!next) {
        return FindResult::failure(std::move(next).error());
    }
    auto owner = std::optional<RetentionRunOwner>{};
    if (*next) {
        auto job       = read_uuid(query.record(), "job_id");
        auto origin    = read_run_origin(query.record(), "origin");
        auto scheduled = read_boolean(query.record(), "schedule_owned");
        if (!job || !origin || !scheduled) {
            return FindResult::failure(invalid_relationship());
        }
        owner = RetentionRunOwner{.job_id = *job, .manual = *origin == RunOrigin::Manual && !*scheduled};
    }
    auto finished = query.finish();
    if (!finished) {
        return FindResult::failure(std::move(finished).error());
    }
    return FindResult::success(owner);
}

auto RetentionOwnerRepository::has_queue(jb::core::Uuid const& id) -> jb::core::Result<bool, jb::core::Error>
{
    return has_row(_database, "SELECT 1 FROM jobu_queues WHERE id = :id", id);
}

auto RetentionOwnerRepository::has_secret_references(jb::core::Uuid const& job)
    -> jb::core::Result<bool, jb::core::Error>
{
    return has_row(_database, "SELECT 1 FROM jobu_secret_refs WHERE job_id = :id LIMIT 1", job);
}

auto RetentionOwnerRepository::delete_owner(RetentionOwnerKind kind, jb::core::Uuid const& id)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    auto sql = std::string{};
    if (kind == RetentionOwnerKind::DeletedJob) {
        sql  = "DELETE FROM jobu_jobs WHERE id = :id AND state = 'deleted' AND ";
        sql += kNoJobRuns;
        sql += " AND NOT EXISTS (SELECT 1 FROM jobu_secret_refs WHERE job_id = :id)";
    }
    else if (kind == RetentionOwnerKind::DeletedQueue) {
        sql  = "DELETE FROM jobu_queues WHERE id = :id AND state = 'deleted' AND ";
        sql += kNoQueueOwners;
    }
    else {
        return RepositoryResult<std::size_t>::failure(invalid_relationship());
    }
    // Scope references protect original creation queues and owning jobs even
    // when the record's resource UUID names a different object.
    sql += " AND NOT EXISTS (SELECT 1 FROM jobu_idempotency WHERE resource_id = :id OR scope_id = :id)";

    jb::db::Query query{_database};
    auto          result = prepare_owner_query(query, sql, id);
    if (!result) {
        return RepositoryResult<std::size_t>::failure(std::move(result).error());
    }
    auto const count    = query.num_rows_affected();
    auto       finished = query.finish();
    if (!finished) {
        return RepositoryResult<std::size_t>::failure(std::move(finished).error());
    }
    if (count < 0 || !std::in_range<std::size_t>(count)) {
        return RepositoryResult<std::size_t>::failure(invalid_relationship());
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(count));
}

} // namespace jb::jobu::detail
