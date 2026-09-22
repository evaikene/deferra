#include "secret_service.hpp"

#include "object_priv.hpp"
#include "payload_template_priv.hpp"
#include "secret_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "transaction.hpp"

#include <optional>
#include <string>
#include <utility>

namespace jb::jobu {

namespace {

template <typename T>
using ServiceResult = jb::core::Result<T, jb::core::Error>;

auto service_error(jb::core::ErrorCategory category, std::string_view code, std::string_view message) -> jb::core::Error
{
    return {.category = category, .code = std::string{code}, .message = std::string{message}};
}

auto invalid_name() -> jb::core::Error
{
    return service_error(jb::core::ErrorCategory::InvalidArgument,
                         "jobu.secret.invalid_name",
                         "Secret name must be a canonical identifier of at most 128 bytes");
}

} // namespace

struct SecretService::Private : jb::core::priv::ObjectPrivate {
    Private(jb::db::Database& database_value, jb::core::TimeSource& time_value)
        : database{database_value}
        , time_source{time_value}
        , secrets{database_value}
    {}

    template <typename T, typename Operation>
    auto invoke(SecretService& owner, detail::StorageOperation context, Operation&& operation) -> ServiceResult<T>
    {
        if (context == detail::StorageOperation::Mutation && mutations_stopped) {
            return ServiceResult<T>::failure(service_error(jb::core::ErrorCategory::Unavailable,
                                                           "jobu.service.stopping",
                                                           "Secret mutations are stopped"));
        }

        // Operation-local queries and RAII transactions must unwind before classification or synchronous slots.
        auto result = std::forward<Operation>(operation)();
        auto fatal  = !result && detail::classify_storage_failure(result.error(), context) ==
                                     detail::StorageFailureDisposition::Fatal;
        if (database.is_poisoned()) {
            // An ordinary not-found/in-use result can conceal failed rollback. Preserve an existing fatal cause.
            if (!fatal) {
                result = ServiceResult<T>::failure(database.last_error().value_or(
                    service_error(jb::core::ErrorCategory::Internal,
                                  "db.connection_failed",
                                  "The database connection is unusable after an unrecoverable failure")));
            }
            fatal = true;
        }

        if (result) {
            if (context == detail::StorageOperation::Mutation) {
                owner.emit(owner.mutation_committed);
            }
            return result;
        }

        // The repository can retain backend detail even on translated conflicts. Never expose it to callers.
        auto& error = result.error();
        if (fatal || error.code.starts_with("db.") || error.code == "jobu.secret.in_use") {
            error = detail::sanitized_storage_error(error, context);
        }
        if (fatal) {
            mutations_stopped = true;
            if (!first_failure) {
                first_failure = error;
                owner.emit(owner.failed, *first_failure);
            }
        }
        return result;
    }

    jb::db::Database&              database;
    jb::core::TimeSource&          time_source;
    detail::SecretRepository       secrets;
    bool                           mutations_stopped{false};
    std::optional<jb::core::Error> first_failure;
};

SecretService::SecretService(jb::db::Database& database, jb::core::TimeSource& time_source, jb::core::Object* parent)
    : Object(*new Private{database, time_source}, parent)
{}

SecretService::~SecretService() = default;

void SecretService::stop_mutations() noexcept
{
    d_ptr<Private>()->mutations_stopped = true;
}

auto SecretService::set(SetSecretRequest request) -> ServiceResult<SecretMetadata>
{
    return d_ptr<Private>()->invoke<SecretMetadata>(*this, detail::StorageOperation::Mutation, [&] {
        return set_impl(request);
    });
}

auto SecretService::list(SecretListRequest const& request) -> ServiceResult<SecretPage>
{
    return d_ptr<Private>()->invoke<SecretPage>(*this, detail::StorageOperation::Read, [&] {
        return list_impl(request);
    });
}

auto SecretService::erase(std::string_view name) -> ServiceResult<void>
{
    return d_ptr<Private>()->invoke<void>(*this, detail::StorageOperation::Mutation, [&] { return erase_impl(name); });
}

auto SecretService::set_impl(SetSecretRequest const& request) -> ServiceResult<SecretMetadata>
{
    if (!detail::is_valid_secret_name(request.name)) {
        return ServiceResult<SecretMetadata>::failure(invalid_name());
    }
    if (request.value.size() > detail::kMaximumSecretValueBytes) {
        return ServiceResult<SecretMetadata>::failure(service_error(jb::core::ErrorCategory::ResourceExhausted,
                                                                    "jobu.secret.too_large",
                                                                    "Secret exceeds its raw byte limit"));
    }

    auto& data  = *d_ptr<Private>();
    auto  begun = jb::db::Transaction::begin(data.database);
    if (!begun) {
        return ServiceResult<SecretMetadata>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();
    auto written     = data.secrets.set(request.name, request.value, data.time_source.utc_now());
    if (!written) {
        return written;
    }
    auto committed = transaction.commit();
    if (!committed) {
        return ServiceResult<SecretMetadata>::failure(std::move(committed).error());
    }
    return written;
}

auto SecretService::list_impl(SecretListRequest const& request) -> ServiceResult<SecretPage>
{
    if (request.limit == 0 || request.limit > 200) {
        return ServiceResult<SecretPage>::failure(service_error(jb::core::ErrorCategory::InvalidArgument,
                                                                "jobu.storage.invalid_limit",
                                                                "Secret page limit must be from 1 through 200"));
    }
    if (request.after_name && !detail::is_valid_secret_name(*request.after_name)) {
        return ServiceResult<SecretPage>::failure(invalid_name());
    }

    auto listed = d_ptr<Private>()->secrets.list_metadata(request.limit + 1, request.after_name);
    if (!listed) {
        return ServiceResult<SecretPage>::failure(std::move(listed).error());
    }
    auto page = SecretPage{.items = std::move(listed).value()};
    if (page.items.size() > request.limit) {
        page.items.resize(request.limit);
        page.next_after_name = page.items.back().name;
    }
    return ServiceResult<SecretPage>::success(std::move(page));
}

auto SecretService::erase_impl(std::string_view name) -> ServiceResult<void>
{
    if (!detail::is_valid_secret_name(name)) {
        return ServiceResult<void>::failure(invalid_name());
    }

    auto& data  = *d_ptr<Private>();
    auto  begun = jb::db::Transaction::begin(data.database);
    if (!begun) {
        return ServiceResult<void>::failure(std::move(begun).error());
    }
    auto transaction = std::move(begun).value();

    // The sole writer holds one transaction across protection and deletion. Detect expected conflicts before SQL
    // constraints reach the fatal classifier. Stage 8.6 extends this boundary to nonterminal run snapshots.
    auto references = data.secrets.reference_count(name);
    if (!references) {
        return ServiceResult<void>::failure(std::move(references).error());
    }
    if (*references != 0) {
        return ServiceResult<void>::failure(service_error(jb::core::ErrorCategory::Conflict,
                                                          "jobu.secret.in_use",
                                                          "Secret is referenced by a current job"));
    }
    auto erased = data.secrets.erase(name);
    if (!erased) {
        return erased;
    }
    return transaction.commit();
}

} // namespace jb::jobu
