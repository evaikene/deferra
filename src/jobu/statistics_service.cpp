#include "statistics_service.hpp"

#include "database.hpp"
#include "object_priv.hpp"
#include "statistics_cursor_priv.hpp"
#include "statistics_repository_priv.hpp"
#include "storage_failure_priv.hpp"
#include "time_source.hpp"
#include "utc_timestamp.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace jb::jobu {

namespace {

template <typename T>
using ServiceResult = jb::core::Result<T, jb::core::Error>;

auto error(jb::core::ErrorCategory category, std::string code, std::string message) -> jb::core::Error
{
    return {.category = category, .code = std::move(code), .message = std::move(message)};
}

auto invalid_request() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::InvalidArgument,
                 "jobu.statistics.invalid_request",
                 "Statistics request is invalid");
}

auto stopped() -> jb::core::Error
{
    return error(jb::core::ErrorCategory::Unavailable, "jobu.service.stopping", "Statistics reads are stopped");
}

auto valid_grouping(StatisticsGroupBy value) -> bool
{
    switch (value) {
        case StatisticsGroupBy::None:
        case StatisticsGroupBy::Queue:
        case StatisticsGroupBy::Job:
        case StatisticsGroupBy::Type:
        case StatisticsGroupBy::Origin:
        case StatisticsGroupBy::State:
            return true;
    }
    return false;
}

auto valid_type(JobType value) -> bool
{
    return value == JobType::Cli || value == JobType::Http;
}

auto resolve_request(StatisticsRequest& request, StatisticsScope scope, jb::core::TimeSource& time_source) -> bool
{
    if ((scope != StatisticsScope::System && scope != StatisticsScope::Queue) || request.limit < 1 ||
        request.limit > 200 || !valid_grouping(request.group_by) || (request.type && !valid_type(*request.type)) ||
        (request.origin && *request.origin != RunOrigin::Scheduled && *request.origin != RunOrigin::Manual) ||
        (scope == StatisticsScope::Queue && !request.queue_id) || (request.queue_id && request.queue_id->is_nil()) ||
        (request.job_id && request.job_id->is_nil())) {
        return false;
    }

    // Resolve both defaults exactly once so a cursor never moves with the wall clock.
    auto const upper = request.planned.to ? *request.planned.to : time_source.utc_now();
    if (!request.planned.from && upper < jb::core::UtcTimePoint::min() + std::chrono::hours{24}) {
        return false;
    }
    auto const lower = request.planned.from ? *request.planned.from : upper - std::chrono::hours{24};
    if (lower >= upper || !format_utc_timestamp(lower) || !format_utc_timestamp(upper)) {
        return false;
    }
    auto const     lower_us = std::chrono::duration_cast<std::chrono::microseconds>(lower.time_since_epoch()).count();
    auto const     upper_us = std::chrono::duration_cast<std::chrono::microseconds>(upper.time_since_epoch()).count();
    auto const     width_us = static_cast<std::uint64_t>(upper_us) - static_cast<std::uint64_t>(lower_us);
    constexpr auto maximum_window_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::hours{31 * 24}).count();
    if (upper_us <= lower_us || width_us > static_cast<std::uint64_t>(maximum_window_us)) {
        return false;
    }
    request.planned = {.from = lower, .to = upper};
    return true;
}

} // namespace

struct StatisticsService::Private : jb::core::priv::ObjectPrivate {
    Private(jb::db::Database&        database_value,
            jb::core::UuidGenerator& uuid_generator,
            jb::core::TimeSource&    time_source)
        : database{database_value}
        , clock{time_source}
        , repository{database_value}
        , cursors{uuid_generator, time_source}
    {}

    template <typename Operation>
    auto invoke(StatisticsService& owner, Operation&& operation) -> ServiceResult<StatisticsPage>
    {
        if (!accepting) {
            return ServiceResult<StatisticsPage>::failure(stopped());
        }

        // Query handles die inside the operation before a synchronous failure slot can stop the daemon.
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
                result = ServiceResult<StatisticsPage>::failure(
                    database.last_error().value_or(error(jb::core::ErrorCategory::Internal,
                                                         "db.connection_failed",
                                                         "Database connection is unusable")));
            }
            fatal = true;
        }
        if (result) {
            return result;
        }
        auto& failure = result.error();
        if (fatal || failure.code.starts_with("db.")) {
            failure = detail::sanitized_storage_error(failure, detail::StorageOperation::Read, origin);
        }
        if (fatal) {
            accepting = false;
            cursors.clear();
            if (!first_failure) {
                first_failure = failure;
                owner.emit(owner.failed, *first_failure);
            }
        }
        return result;
    }

    jb::db::Database&              database;
    jb::core::TimeSource&          clock;
    detail::StatisticsRepository   repository;
    detail::StatisticsCursorStore  cursors;
    bool                           accepting{true};
    std::optional<jb::core::Error> first_failure;
};

StatisticsService::StatisticsService(jb::db::Database&        database,
                                     jb::core::UuidGenerator& uuid_generator,
                                     jb::core::TimeSource&    time_source,
                                     jb::core::Object*        parent)
    : Object(*new Private{database, uuid_generator, time_source}, parent)
{}

StatisticsService::~StatisticsService() = default;

auto StatisticsService::read(StatisticsListRequest const& request, StatisticsScope scope)
    -> ServiceResult<StatisticsPage>
{
    auto* data = d_ptr<Private>();
    return data->invoke(*this, [&]() -> ServiceResult<StatisticsPage> {
        auto query    = StatisticsRequest{};
        auto previous = std::optional<detail::StatisticsCursorState>{};
        auto after    = std::optional<StatisticsGroupKey>{};
        if (auto const* initial = std::get_if<StatisticsRequest>(&request)) {
            query = *initial;
            if (!resolve_request(query, scope, data->clock)) {
                return ServiceResult<StatisticsPage>::failure(invalid_request());
            }
        }
        else {
            auto loaded = data->cursors.get(std::get<CursorRequest>(request).cursor, scope);
            if (!loaded) {
                return ServiceResult<StatisticsPage>::failure(std::move(loaded).error());
            }
            previous = std::move(loaded).value();
            query    = previous->request;
            after    = previous->after;
        }

        auto keys = data->repository.list_groups(query, after, query.limit + 1U);
        if (!keys) {
            return ServiceResult<StatisticsPage>::failure(std::move(keys).error());
        }
        auto page = StatisticsPage{.window = query.planned, .group_by = query.group_by};
        for (auto index = std::size_t{0}; index < keys->size() && index < query.limit; ++index) {
            auto aggregate = data->repository.aggregate(query, (*keys)[index]);
            if (!aggregate) {
                return ServiceResult<StatisticsPage>::failure(std::move(aggregate).error());
            }
            page.groups.push_back(std::move(aggregate).value());
        }
        if (page.groups.size() < keys->size()) {
            auto next = previous ? data->cursors.advance(*previous, page.groups.back().key)
                                 : data->cursors.start(query, scope, page.groups.back().key);
            if (!next) {
                return ServiceResult<StatisticsPage>::failure(std::move(next).error());
            }
            page.next_cursor = std::move(next).value();
        }
        return ServiceResult<StatisticsPage>::success(std::move(page));
    });
}

void StatisticsService::shutdown() noexcept
{
    auto* data      = d_ptr<Private>();
    data->accepting = false;
    data->cursors.clear();
}

} // namespace jb::jobu
