#include "idempotency_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "queue_validation_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumRetentionBatch = 1000;

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_record(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.idempotency.invalid_record",
                                    "Stored idempotency data is invalid");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto invalid_limit() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.storage.invalid_limit",
                            "Repository limit is outside its supported range");
}

auto conflict(jb::core::Error const& cause) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Conflict,
                                    "jobu.idempotency.conflict",
                                    "The idempotency key was already used for a different request");
    error.detail = "cause=" + cause.code;
    return error;
}

auto bind_all(jb::db::Query& query, std::vector<std::pair<std::string_view, jb::db::Value>> values)
    -> RepositoryResult<void>
{
    for (auto& [placeholder, value] : values) {
        auto bound = query.bind_value(placeholder, std::move(value));
        if (!bound) {
            return RepositoryResult<void>::failure(std::move(bound).error());
        }
    }
    return RepositoryResult<void>::success();
}

auto affected_rows(jb::db::Query const& query) -> RepositoryResult<std::size_t>
{
    auto const count = query.num_rows_affected();
    if (count < 0 || !std::in_range<std::size_t>(count)) {
        return RepositoryResult<std::size_t>::failure(invalid_record("invalid_affected_row_count"));
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(count));
}

auto decode_record(jb::db::Record const& row) -> RepositoryResult<IdempotencyRecord>
{
    auto method = read_text(row, "idempotency_method");
    if (!method) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(method).error());
    }
    auto scope_id = read_uuid(row, "idempotency_scope_id");
    if (!scope_id) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(scope_id).error());
    }
    auto key = read_text(row, "idempotency_key");
    if (!key) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(key).error());
    }
    auto request = read_text(row, "idempotency_request_json");
    if (!request) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(request).error());
    }
    auto result = read_text(row, "idempotency_result_json");
    if (!result) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(result).error());
    }
    auto resource_id = read_uuid(row, "idempotency_resource_id");
    if (!resource_id) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(resource_id).error());
    }
    auto created_at = read_timestamp(row, "idempotency_created_at_us");
    if (!created_at) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(created_at).error());
    }
    auto expires_at = read_optional_timestamp(row, "idempotency_expires_at_us");
    if (!expires_at) {
        return RepositoryResult<IdempotencyRecord>::failure(std::move(expires_at).error());
    }
    if (method->empty() || !is_valid_idempotency_key(*key) || request->empty() || result->empty()) {
        return RepositoryResult<IdempotencyRecord>::failure(invalid_record("invalid_text_field"));
    }
    return RepositoryResult<IdempotencyRecord>::success({
        .method       = std::move(method).value(),
        .scope_id     = *scope_id,
        .key          = std::move(key).value(),
        .request_json = std::move(request).value(),
        .result_json  = std::move(result).value(),
        .resource_id  = *resource_id,
        .created_at   = *created_at,
        .expires_at   = *expires_at,
    });
}

} // anonymous namespace

IdempotencyRepository::IdempotencyRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto IdempotencyRepository::find(std::string_view method, jb::core::Uuid const& scope_id, std::string_view key)
    -> jb::core::Result<std::optional<IdempotencyRecord>, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared =
        query.prepare("SELECT method AS idempotency_method, scope_id AS idempotency_scope_id, key AS idempotency_key, "
                      "request_json AS idempotency_request_json, result_json AS idempotency_result_json, "
                      "resource_id AS idempotency_resource_id, created_at_us AS idempotency_created_at_us, "
                      "expires_at_us AS idempotency_expires_at_us FROM jobu_idempotency "
                      "WHERE method = :method AND scope_id = :scope_id AND key = :key");
    if (!prepared) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":method",   jb::db::make_text(method)},
                              {":scope_id", uuid_to_storage(scope_id)},
                              {":key",      jb::db::make_text(key)   },
    });
    if (!bound) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::success(std::nullopt);
    }
    auto decoded = decode_record(query.record());
    if (!decoded) {
        return RepositoryResult<std::optional<IdempotencyRecord>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<IdempotencyRecord>>::success(std::move(decoded).value());
}

auto IdempotencyRepository::insert(IdempotencyRecord const& record) -> jb::core::Result<void, jb::core::Error>
{
    auto created = timestamp_to_storage(record.created_at);
    if (!created) {
        return RepositoryResult<void>::failure(std::move(created).error());
    }
    auto expires = record.expires_at ? timestamp_to_storage(*record.expires_at)
                                     : RepositoryResult<jb::db::Value>::success(jb::db::Null{});
    if (!expires) {
        return RepositoryResult<void>::failure(std::move(expires).error());
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare(
        "INSERT INTO jobu_idempotency(method, scope_id, key, request_json, result_json, resource_id, created_at_us, "
        "expires_at_us) VALUES(:method, :scope_id, :key, :request_json, :result_json, :resource_id, :created_at_us, "
        ":expires_at_us)");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = bind_all(query,
                          {
                              {":method",        jb::db::make_text(record.method)      },
                              {":scope_id",      uuid_to_storage(record.scope_id)      },
                              {":key",           jb::db::make_text(record.key)         },
                              {":request_json",  jb::db::make_text(record.request_json)},
                              {":result_json",   jb::db::make_text(record.result_json) },
                              {":resource_id",   uuid_to_storage(record.resource_id)   },
                              {":created_at_us", std::move(created).value()            },
                              {":expires_at_us", std::move(expires).value()            },
    });
    if (!bound) {
        return bound;
    }
    auto executed = query.exec();
    if (!executed && executed.error().code == "db.constraint.unique") {
        return RepositoryResult<void>::failure(conflict(executed.error()));
    }
    return executed;
}

auto IdempotencyRepository::list_referencing(jb::core::Uuid const&                owner,
                                             std::size_t                          limit,
                                             std::optional<IdempotencyKey> const& after)
    -> jb::core::Result<std::vector<IdempotencyRecord>, jb::core::Error>
{
    using PageResult = RepositoryResult<std::vector<IdempotencyRecord>>;
    if (limit == 0 || limit > kMaximumRetentionBatch) {
        return PageResult::failure(invalid_limit());
    }

    // Page reference keys through each owner index before fetching documents.
    // Sorting the UNION's full JSON rows would materialize an owner's entire
    // replay history before LIMIT. UNION deduplicates the two roles by primary key.
    auto continuation = std::string{};
    if (after) {
        continuation = " AND (method, scope_id, key) > (:after_method, :after_scope, :after_key)";
    }
    auto keys = "SELECT method, scope_id, key FROM jobu_idempotency WHERE resource_id = :owner" + continuation +
                " UNION SELECT method, scope_id, key FROM jobu_idempotency WHERE scope_id = :owner" + continuation +
                " ORDER BY method, scope_id, key LIMIT :limit";
    auto sql =
        std::string{"SELECT method AS idempotency_method, scope_id AS idempotency_scope_id, key AS idempotency_key, "
                    "request_json AS idempotency_request_json, result_json AS idempotency_result_json, "
                    "resource_id AS idempotency_resource_id, created_at_us AS idempotency_created_at_us, "
                    "expires_at_us AS idempotency_expires_at_us FROM ("} +
        keys + ") AS reference_keys JOIN jobu_idempotency USING (method, scope_id, key) ORDER BY method, scope_id, key";

    jb::db::Query query{_database};
    auto          result = query.prepare(sql);
    if (result) {
        result = bind_all(query,
                          {
                              {":owner", uuid_to_storage(owner)          },
                              {":limit", static_cast<std::int64_t>(limit)}
        });
    }
    if (result && after) {
        result = bind_all(query,
                          {
                              {":after_method", jb::db::make_text(after->method)},
                              {":after_scope",  uuid_to_storage(after->scope_id)},
                              {":after_key",    jb::db::make_text(after->key)   }
        });
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return PageResult::failure(std::move(result).error());
    }

    auto records = std::vector<IdempotencyRecord>{};
    records.reserve(limit);
    for (;;) {
        auto next = query.next();
        if (!next) {
            return PageResult::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto decoded = decode_record(query.record());
        if (!decoded) {
            return PageResult::failure(std::move(decoded).error());
        }
        records.push_back(std::move(decoded).value());
    }
    auto finished = query.finish();
    if (!finished) {
        return PageResult::failure(std::move(finished).error());
    }
    return PageResult::success(std::move(records));
}

auto IdempotencyRepository::erase_matching(IdempotencyRecord const& record)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          result = query.prepare("DELETE FROM jobu_idempotency WHERE method = :method AND scope_id = :scope_id "
                                         "AND key = :key AND resource_id = :resource_id");
    if (result) {
        result = bind_all(query,
                          {
                              {":method",      jb::db::make_text(record.method)   },
                              {":scope_id",    uuid_to_storage(record.scope_id)   },
                              {":key",         jb::db::make_text(record.key)      },
                              {":resource_id", uuid_to_storage(record.resource_id)}
        });
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return RepositoryResult<std::size_t>::failure(std::move(result).error());
    }
    auto count    = affected_rows(query);
    auto finished = query.finish();
    if (!finished) {
        return RepositoryResult<std::size_t>::failure(std::move(finished).error());
    }
    return count;
}

} // namespace jb::jobu::detail
