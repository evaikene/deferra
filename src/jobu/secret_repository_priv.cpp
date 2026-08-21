#include "secret_repository_priv.hpp"

#include "attribute.hpp"
#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "text_validation_priv.hpp"
#include "value.hpp"

#include <cstdint>
#include <set>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using RepositoryResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t kMaximumSecretNameBytes = 128;
constexpr std::size_t kMaximumMetadataRows    = 200;
constexpr std::size_t kMaximumReferences      = 256;

auto repository_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message)
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::string{code},
        .message  = std::string{message},
    };
}

auto invalid_name() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.secret.invalid_name",
                            "Secret name must be a canonical identifier of at most 128 bytes");
}

auto not_found() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::NotFound, "jobu.secret.not_found", "Secret was not found");
}

auto in_use(jb::core::Error const& cause) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Conflict,
                                    "jobu.secret.in_use",
                                    "Secret is referenced by a current job");
    error.detail = "cause=" + cause.code;
    return error;
}

auto invalid_limit() -> jb::core::Error
{
    return repository_error(jb::core::ErrorCategory::InvalidArgument,
                            "jobu.storage.invalid_limit",
                            "Repository limit is outside its supported range");
}

auto invalid_record(std::string_view reason) -> jb::core::Error
{
    auto error   = repository_error(jb::core::ErrorCategory::Internal,
                                    "jobu.storage.invariant",
                                    "Persisted secret data violates a JobU invariant");
    error.detail = "reason=" + std::string{reason};
    return error;
}

auto valid_name(std::string_view name) noexcept -> bool
{
    return name.size() <= kMaximumSecretNameBytes && is_valid_attribute_name(name);
}

auto valid_field_path(std::string_view path) noexcept -> bool
{
    return !path.empty() && is_valid_utf8(path) && !has_ascii_control(path);
}

auto affected_rows(jb::db::Query const& query) -> RepositoryResult<std::size_t>
{
    auto const count = query.num_rows_affected();
    if (count < 0 || !std::in_range<std::size_t>(count)) {
        return RepositoryResult<std::size_t>::failure(invalid_record("invalid_affected_row_count"));
    }
    return RepositoryResult<std::size_t>::success(static_cast<std::size_t>(count));
}

auto decode_metadata(jb::db::Record const& row) -> RepositoryResult<SecretMetadata>
{
    auto name = read_text(row, "secret_name");
    if (!name) {
        return RepositoryResult<SecretMetadata>::failure(std::move(name).error());
    }
    if (!valid_name(*name)) {
        return RepositoryResult<SecretMetadata>::failure(invalid_record("invalid_name"));
    }
    auto created = read_timestamp(row, "secret_created_at_us");
    if (!created) {
        return RepositoryResult<SecretMetadata>::failure(std::move(created).error());
    }
    auto updated = read_timestamp(row, "secret_updated_at_us");
    if (!updated) {
        return RepositoryResult<SecretMetadata>::failure(std::move(updated).error());
    }
    return RepositoryResult<SecretMetadata>::success({
        .name       = std::move(name).value(),
        .created_at = *created,
        .updated_at = *updated,
    });
}

auto find_metadata(jb::db::Database& database, std::string_view name) -> RepositoryResult<std::optional<SecretMetadata>>
{
    jb::db::Query query{database};
    auto          prepared = query.prepare(
        "SELECT name AS secret_name, created_at_us AS secret_created_at_us, updated_at_us AS secret_updated_at_us "
        "FROM jobu_secrets WHERE name = :name");
    if (!prepared) {
        return RepositoryResult<std::optional<SecretMetadata>>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":name", jb::db::make_text(name));
    if (!bound) {
        return RepositoryResult<std::optional<SecretMetadata>>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::optional<SecretMetadata>>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::optional<SecretMetadata>>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::optional<SecretMetadata>>::success(std::nullopt);
    }
    auto decoded = decode_metadata(query.record());
    if (!decoded) {
        return RepositoryResult<std::optional<SecretMetadata>>::failure(std::move(decoded).error());
    }
    return RepositoryResult<std::optional<SecretMetadata>>::success(std::move(decoded).value());
}

} // anonymous namespace

SecretRepository::SecretRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto SecretRepository::set(std::string_view name, jb::core::ByteView value, jb::core::UtcTimePoint updated_at)
    -> jb::core::Result<SecretMetadata, jb::core::Error>
{
    if (!valid_name(name)) {
        return RepositoryResult<SecretMetadata>::failure(invalid_name());
    }
    auto timestamp = timestamp_to_storage(updated_at);
    if (!timestamp) {
        return RepositoryResult<SecretMetadata>::failure(std::move(timestamp).error());
    }
    auto existing = find_metadata(_database, name);
    if (!existing) {
        return RepositoryResult<SecretMetadata>::failure(std::move(existing).error());
    }
    jb::db::Query query{_database};
    if (existing->has_value()) {
        auto prepared = query.prepare(
            "UPDATE jobu_secrets SET value_blob = :value_blob, updated_at_us = :updated_at_us WHERE name = :name");
        if (!prepared) {
            return RepositoryResult<SecretMetadata>::failure(std::move(prepared).error());
        }
        auto bound_value = query.bind_value(":value_blob", jb::db::make_blob(value));
        if (!bound_value) {
            return RepositoryResult<SecretMetadata>::failure(std::move(bound_value).error());
        }
        auto bound_time = query.bind_value(":updated_at_us", timestamp.value());
        if (!bound_time) {
            return RepositoryResult<SecretMetadata>::failure(std::move(bound_time).error());
        }
        auto bound_name = query.bind_value(":name", jb::db::make_text(name));
        if (!bound_name) {
            return RepositoryResult<SecretMetadata>::failure(std::move(bound_name).error());
        }
        auto executed = query.exec();
        if (!executed) {
            return RepositoryResult<SecretMetadata>::failure(std::move(executed).error());
        }
        auto affected = affected_rows(query);
        if (!affected) {
            return RepositoryResult<SecretMetadata>::failure(std::move(affected).error());
        }
        if (*affected != 1U) {
            return RepositoryResult<SecretMetadata>::failure(invalid_record("secret_update_count"));
        }
        auto metadata       = std::move(**existing);
        metadata.updated_at = updated_at;
        return RepositoryResult<SecretMetadata>::success(std::move(metadata));
    }

    auto prepared = query.prepare("INSERT INTO jobu_secrets(name, value_blob, created_at_us, updated_at_us) "
                                  "VALUES(:name, :value_blob, :created_at_us, :updated_at_us)");
    if (!prepared) {
        return RepositoryResult<SecretMetadata>::failure(std::move(prepared).error());
    }
    auto bound_name = query.bind_value(":name", jb::db::make_text(name));
    if (!bound_name) {
        return RepositoryResult<SecretMetadata>::failure(std::move(bound_name).error());
    }
    auto bound_value = query.bind_value(":value_blob", jb::db::make_blob(value));
    if (!bound_value) {
        return RepositoryResult<SecretMetadata>::failure(std::move(bound_value).error());
    }
    auto bound_created = query.bind_value(":created_at_us", timestamp.value());
    if (!bound_created) {
        return RepositoryResult<SecretMetadata>::failure(std::move(bound_created).error());
    }
    auto bound_updated = query.bind_value(":updated_at_us", timestamp.value());
    if (!bound_updated) {
        return RepositoryResult<SecretMetadata>::failure(std::move(bound_updated).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<SecretMetadata>::failure(std::move(executed).error());
    }
    return RepositoryResult<SecretMetadata>::success({
        .name       = std::string{name},
        .created_at = updated_at,
        .updated_at = updated_at,
    });
}

auto SecretRepository::list_metadata(std::size_t limit, std::optional<std::string_view> after_name)
    -> jb::core::Result<std::vector<SecretMetadata>, jb::core::Error>
{
    if (limit == 0 || limit > kMaximumMetadataRows || !std::in_range<std::int64_t>(limit)) {
        return RepositoryResult<std::vector<SecretMetadata>>::failure(invalid_limit());
    }
    auto sql = std::string{
        "SELECT name AS secret_name, created_at_us AS secret_created_at_us, updated_at_us AS secret_updated_at_us "
        "FROM jobu_secrets"};
    if (after_name) {
        sql += " WHERE name > :after_name";
    }
    sql += " ORDER BY name ASC LIMIT :limit";
    jb::db::Query query{_database};
    auto          prepared = query.prepare(sql);
    if (!prepared) {
        return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(prepared).error());
    }
    if (after_name) {
        auto bound = query.bind_value(":after_name", jb::db::make_text(*after_name));
        if (!bound) {
            return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(bound).error());
        }
    }
    auto bound_limit = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    if (!bound_limit) {
        return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(bound_limit).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(executed).error());
    }
    auto result = std::vector<SecretMetadata>{};
    result.reserve(limit);
    while (true) {
        auto next = query.next();
        if (!next) {
            return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(next).error());
        }
        if (!*next) {
            break;
        }
        auto metadata = decode_metadata(query.record());
        if (!metadata) {
            return RepositoryResult<std::vector<SecretMetadata>>::failure(std::move(metadata).error());
        }
        result.push_back(std::move(metadata).value());
    }
    return RepositoryResult<std::vector<SecretMetadata>>::success(std::move(result));
}

auto SecretRepository::erase(std::string_view name) -> jb::core::Result<void, jb::core::Error>
{
    if (!valid_name(name)) {
        return RepositoryResult<void>::failure(invalid_name());
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare("DELETE FROM jobu_secrets WHERE name = :name");
    if (!prepared) {
        return RepositoryResult<void>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":name", jb::db::make_text(name));
    if (!bound) {
        return RepositoryResult<void>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed &&
        (executed.error().code == "db.constraint.foreign_key" || executed.error().code == "db.constraint")) {
        return RepositoryResult<void>::failure(in_use(executed.error()));
    }
    if (!executed) {
        return RepositoryResult<void>::failure(std::move(executed).error());
    }
    auto affected = affected_rows(query);
    if (!affected) {
        return RepositoryResult<void>::failure(std::move(affected).error());
    }
    if (*affected == 0) {
        return RepositoryResult<void>::failure(not_found());
    }
    if (*affected != 1U) {
        return RepositoryResult<void>::failure(invalid_record("secret_delete_count"));
    }
    return RepositoryResult<void>::success();
}

auto SecretRepository::replace_references_for_job(jb::core::Uuid const&            job_id,
                                                  std::span<SecretReference const> references)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    if (references.size() > kMaximumReferences) {
        return RepositoryResult<std::size_t>::failure(invalid_limit());
    }
    auto unique = std::set<std::pair<std::string_view, std::string_view>>{};
    for (auto const& reference : references) {
        if (!valid_name(reference.secret_name) || !valid_field_path(reference.field_path) ||
            !unique.emplace(reference.secret_name, reference.field_path).second) {
            return RepositoryResult<std::size_t>::failure(invalid_record("invalid_secret_reference"));
        }
    }
    {
        jb::db::Query query{_database};
        auto          prepared = query.prepare("DELETE FROM jobu_secret_refs WHERE job_id = :job_id");
        if (!prepared) {
            return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
        }
        auto bound = query.bind_value(":job_id", uuid_to_storage(job_id));
        if (!bound) {
            return RepositoryResult<std::size_t>::failure(std::move(bound).error());
        }
        auto executed = query.exec();
        if (!executed) {
            return RepositoryResult<std::size_t>::failure(std::move(executed).error());
        }
    }
    auto inserted = std::size_t{0};
    for (auto const& reference : references) {
        jb::db::Query query{_database};
        auto          prepared = query.prepare(
            "INSERT INTO jobu_secret_refs(secret_name, job_id, field_path) VALUES(:secret_name, :job_id, :field_path)");
        if (!prepared) {
            return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
        }
        auto bound_name = query.bind_value(":secret_name", jb::db::make_text(reference.secret_name));
        if (!bound_name) {
            return RepositoryResult<std::size_t>::failure(std::move(bound_name).error());
        }
        auto bound_job = query.bind_value(":job_id", uuid_to_storage(job_id));
        if (!bound_job) {
            return RepositoryResult<std::size_t>::failure(std::move(bound_job).error());
        }
        auto bound_path = query.bind_value(":field_path", jb::db::make_text(reference.field_path));
        if (!bound_path) {
            return RepositoryResult<std::size_t>::failure(std::move(bound_path).error());
        }
        auto executed = query.exec();
        if (!executed) {
            return RepositoryResult<std::size_t>::failure(std::move(executed).error());
        }
        ++inserted;
    }
    return RepositoryResult<std::size_t>::success(inserted);
}

auto SecretRepository::erase_references_for_queue(jb::core::Uuid const& queue_id)
    -> jb::core::Result<std::size_t, jb::core::Error>
{
    jb::db::Query query{_database};
    auto          prepared = query.prepare("DELETE FROM jobu_secret_refs WHERE job_id IN (SELECT id FROM jobu_jobs "
                                           "WHERE queue_id = :queue_id AND state <> 'deleted')");
    if (!prepared) {
        return RepositoryResult<std::size_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":queue_id", uuid_to_storage(queue_id));
    if (!bound) {
        return RepositoryResult<std::size_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::size_t>::failure(std::move(executed).error());
    }
    return affected_rows(query);
}

auto SecretRepository::reference_count(std::string_view name) -> jb::core::Result<std::uint64_t, jb::core::Error>
{
    if (!valid_name(name)) {
        return RepositoryResult<std::uint64_t>::failure(invalid_name());
    }
    jb::db::Query query{_database};
    auto          prepared = query.prepare("SELECT COUNT(*) AS reference_count FROM jobu_secret_refs "
                                           "WHERE secret_name = :secret_name");
    if (!prepared) {
        return RepositoryResult<std::uint64_t>::failure(std::move(prepared).error());
    }
    auto bound = query.bind_value(":secret_name", jb::db::make_text(name));
    if (!bound) {
        return RepositoryResult<std::uint64_t>::failure(std::move(bound).error());
    }
    auto executed = query.exec();
    if (!executed) {
        return RepositoryResult<std::uint64_t>::failure(std::move(executed).error());
    }
    auto next = query.next();
    if (!next) {
        return RepositoryResult<std::uint64_t>::failure(std::move(next).error());
    }
    if (!*next) {
        return RepositoryResult<std::uint64_t>::failure(invalid_record("missing_reference_count"));
    }
    auto const* value = query.value("reference_count");
    auto const* count = value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    if (count == nullptr || *count < 0) {
        return RepositoryResult<std::uint64_t>::failure(invalid_record("invalid_reference_count"));
    }
    return RepositoryResult<std::uint64_t>::success(static_cast<std::uint64_t>(*count));
}

} // namespace jb::jobu::detail
