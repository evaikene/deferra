#include "history_service.hpp"

#include "database.hpp"
#include "history_cursor_priv.hpp"
#include "history_json.hpp"
#include "history_repository_priv.hpp"
#include "json.hpp"
#include "object_priv.hpp"
#include "storage_failure_priv.hpp"

#include <optional>
#include <string>
#include <utility>

namespace {

template <typename T>
using ServiceResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t result_budget_bytes = std::size_t{512} * 1024U;
// The complete page wrapper and a UUID cursor use less than this reserve.
constexpr std::size_t page_wrapper_bytes  = 128;

auto error(jb::core::ErrorCategory category, std::string_view code, std::string_view message) -> jb::core::Error
{
    return {.category = category, .code = std::string{code}, .message = std::string{message}};
}

auto stopped() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::Unavailable, "jobu.service.stopping", "History reads are stopped");
}

auto invalid_request() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::InvalidArgument,
                 "jobu.history.invalid_request",
                 "History request is invalid");
}

auto too_large() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::ResourceExhausted,
                 "jobu.response.too_large",
                 "History result exceeds its response limit");
}

auto invalid_stored_summary() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::Internal, "jobu.storage.invariant", "Persisted history cannot be encoded");
}

auto serialized_size(jb::core::JsonValue const& value) -> ServiceResult<std::size_t>
{
    auto encoded = jb::core::serialize_json(value);
    if (!encoded) {
        return ServiceResult<std::size_t>::failure(invalid_stored_summary());
    }
    return ServiceResult<std::size_t>::success(encoded->size());
}

auto valid_query(jb::jobu::RunQuery const& query) -> bool
{
    return jb::jobu::run_list_request_to_json(jb::jobu::RunListRequest{query}).has_value();
}

auto valid_query(jb::jobu::AttemptQuery const& query) -> bool
{
    return jb::jobu::attempt_list_request_to_json(jb::jobu::AttemptListRequest{query}).has_value();
}

} // namespace

namespace jb::jobu {

struct HistoryService::Private : jb::core::priv::ObjectPrivate {
    Private(jb::db::Database&        database_value,
            AttributeRegistry const& attributes_value,
            jb::core::UuidGenerator& uuid_generator,
            jb::core::TimeSource&    time_source)
        : database{database_value}
        , attributes{attributes_value}
        , repository{database_value, attributes_value}
        , cursors{uuid_generator, time_source}
    {}

    template <typename T, typename Operation>
    auto invoke(HistoryService& owner, Operation&& operation) -> ServiceResult<T>
    {
        if (!accepting) {
            return ServiceResult<T>::failure(stopped());
        }

        // The operation owns all query handles. Deliver a fatal signal only after it returns and releases them.
        auto result = std::forward<Operation>(operation)();
        auto origin = detail::StorageFailureOrigin::Operation;
        auto fatal  = false;
        if (!result) {
            origin = result.error().code.starts_with("jobu.storage.") ? detail::StorageFailureOrigin::PersistedData
                                                                      : detail::StorageFailureOrigin::Operation;
            fatal  = detail::classify_storage_failure(result.error(), detail::StorageOperation::Read, origin) ==
                     detail::StorageFailureDisposition::Fatal;
        }
        if (database.is_poisoned()) {
            if (!fatal) {
                result =
                    ServiceResult<T>::failure(database.last_error().value_or(error(jb::core::ErrorCategory::Internal,
                                                                                   "db.connection_failed",
                                                                                   "Database connection is unusable")));
            }
            fatal = true;
        }
        if (result) {
            return result;
        }

        auto& error_value = result.error();
        if (fatal || error_value.code.starts_with("db.")) {
            error_value = detail::sanitized_storage_error(error_value, detail::StorageOperation::Read, origin);
        }
        if (fatal) {
            accepting = false;
            cursors.clear();
            if (!first_failure) {
                first_failure = error_value;
                owner.emit(owner.failed, *first_failure);
            }
        }
        return result;
    }

    jb::db::Database&              database;
    AttributeRegistry const&       attributes;
    detail::HistoryRepository      repository;
    detail::HistoryCursorStore     cursors;
    bool                           accepting{true};
    std::optional<jb::core::Error> first_failure;
};

HistoryService::HistoryService(jb::db::Database&        database,
                               AttributeRegistry const& attributes,
                               jb::core::UuidGenerator& uuid_generator,
                               jb::core::TimeSource&    time_source,
                               jb::core::Object*        parent)
    : Object(*new Private{database, attributes, uuid_generator, time_source}, parent)
{}

HistoryService::~HistoryService() = default;

auto HistoryService::get_run(jb::core::Uuid const& id) -> ServiceResult<RunDetails>
{
    auto* data = d_ptr<Private>();
    return data->invoke<RunDetails>(*this, [&] {
        auto found = data->repository.get_run(id);
        if (!found) {
            return ServiceResult<RunDetails>::failure(std::move(found).error());
        }
        if (!found->has_value()) {
            return ServiceResult<RunDetails>::failure(
                error(jb::core::ErrorCategory::NotFound, "jobu.run.not_found", "Run was not found"));
        }
        return ServiceResult<RunDetails>::success(std::move(**found));
    });
}

auto HistoryService::list_runs(RunListRequest const& request) -> ServiceResult<RunPage>
{
    auto* data = d_ptr<Private>();
    return data->invoke<RunPage>(*this, [&] {
        auto query    = RunQuery{};
        auto previous = std::optional<detail::RunCursorState>{};
        auto after    = std::optional<detail::RunCursorKey>{};

        if (auto const* initial = std::get_if<RunQuery>(&request)) {
            if (!valid_query(*initial)) {
                return ServiceResult<RunPage>::failure(invalid_request());
            }
            query = *initial;
        }
        else {
            auto loaded = data->cursors.get_run(std::get<CursorRequest>(request).cursor);
            if (!loaded) {
                return ServiceResult<RunPage>::failure(std::move(loaded).error());
            }
            previous = std::move(loaded).value();
            query    = previous->query;
            after    = previous->after;
        }

        auto rows = data->repository.list_runs(query, after, query.limit + 1U);
        if (!rows) {
            return ServiceResult<RunPage>::failure(std::move(rows).error());
        }

        auto page  = RunPage{};
        auto bytes = page_wrapper_bytes;
        for (auto const& row : *rows) {
            if (page.items.size() == query.limit) {
                break;
            }
            auto encoded = run_summary_to_json(row);
            if (!encoded) {
                return ServiceResult<RunPage>::failure(invalid_stored_summary());
            }
            auto size = serialized_size(*encoded);
            if (!size) {
                return ServiceResult<RunPage>::failure(std::move(size).error());
            }
            if (*size + 1U > result_budget_bytes - bytes) {
                break;
            }
            bytes += *size + 1U;
            page.items.push_back(row);
        }
        if (page.items.empty() && !rows->empty()) {
            return ServiceResult<RunPage>::failure(too_large());
        }

        if (page.items.size() < rows->size()) {
            // The lookahead is never a continuation boundary; the first un-emitted row remains reachable.
            auto const& last = page.items.back();
            auto        key  = detail::RunCursorKey{.planned_at = last.planned_at, .id = last.id};
            auto next = previous ? data->cursors.advance_run(*previous, key) : data->cursors.start_run(query, key);
            if (!next) {
                return ServiceResult<RunPage>::failure(std::move(next).error());
            }
            page.next_cursor = std::move(next).value();
        }
        return ServiceResult<RunPage>::success(std::move(page));
    });
}

auto HistoryService::get_attempt(AttemptKey const& key) -> ServiceResult<AttemptDetails>
{
    auto* data = d_ptr<Private>();
    return data->invoke<AttemptDetails>(*this, [&] {
        if (key.attempt_number == 0) {
            return ServiceResult<AttemptDetails>::failure(invalid_request());
        }
        auto found = data->repository.get_attempt(key);
        if (!found) {
            return ServiceResult<AttemptDetails>::failure(std::move(found).error());
        }
        if (!found->has_value()) {
            return ServiceResult<AttemptDetails>::failure(
                error(jb::core::ErrorCategory::NotFound, "jobu.attempt.not_found", "Attempt was not found"));
        }
        return ServiceResult<AttemptDetails>::success(std::move(**found));
    });
}

auto HistoryService::list_attempts(AttemptListRequest const& request) -> ServiceResult<AttemptPage>
{
    auto* data = d_ptr<Private>();
    return data->invoke<AttemptPage>(*this, [&] {
        auto query    = AttemptQuery{};
        auto previous = std::optional<detail::AttemptCursorState>{};
        auto before   = std::optional<AttemptNumber>{};

        if (auto const* initial = std::get_if<AttemptQuery>(&request)) {
            if (!valid_query(*initial)) {
                return ServiceResult<AttemptPage>::failure(invalid_request());
            }
            query = *initial;
        }
        else {
            auto loaded = data->cursors.get_attempt(std::get<CursorRequest>(request).cursor);
            if (!loaded) {
                return ServiceResult<AttemptPage>::failure(std::move(loaded).error());
            }
            previous = std::move(loaded).value();
            query    = previous->query;
            before   = previous->before_attempt_number;
        }

        auto rows = data->repository.list_attempts(query, before, query.limit + 1U);
        if (!rows) {
            return ServiceResult<AttemptPage>::failure(std::move(rows).error());
        }

        auto page  = AttemptPage{};
        auto bytes = page_wrapper_bytes;
        for (auto const& row : *rows) {
            if (page.items.size() == query.limit) {
                break;
            }
            auto encoded = attempt_summary_to_json(row);
            if (!encoded) {
                return ServiceResult<AttemptPage>::failure(invalid_stored_summary());
            }
            auto size = serialized_size(*encoded);
            if (!size) {
                return ServiceResult<AttemptPage>::failure(std::move(size).error());
            }
            if (*size + 1U > result_budget_bytes - bytes) {
                break;
            }
            bytes += *size + 1U;
            page.items.push_back(row);
        }
        if (page.items.empty() && !rows->empty()) {
            return ServiceResult<AttemptPage>::failure(too_large());
        }

        if (page.items.size() < rows->size()) {
            auto const last = page.items.back().attempt_number;
            auto       next =
                previous ? data->cursors.advance_attempt(*previous, last) : data->cursors.start_attempt(query, last);
            if (!next) {
                return ServiceResult<AttemptPage>::failure(std::move(next).error());
            }
            page.next_cursor = std::move(next).value();
        }
        return ServiceResult<AttemptPage>::success(std::move(page));
    });
}

void HistoryService::shutdown() noexcept
{
    auto* data      = d_ptr<Private>();
    data->accepting = false;
    data->cursors.clear();
}

} // namespace jb::jobu
