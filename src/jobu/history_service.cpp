#include "history_service.hpp"

#include "database.hpp"
#include "history_cursor_priv.hpp"
#include "history_json.hpp"
#include "history_repository_priv.hpp"
#include "json.hpp"
#include "object_priv.hpp"
#include "storage_failure_priv.hpp"
#include "text_validation_priv.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace {

template <typename T>
using ServiceResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t result_budget_bytes        = std::size_t{512} * 1024U;
// The complete page wrapper and a UUID cursor use less than this reserve.
constexpr std::size_t page_wrapper_bytes         = 128;
constexpr std::size_t maximum_output_slice_bytes = 65'536;

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

auto valid_output_request(jb::jobu::AttemptOutputRequest const& request) -> bool
{
    auto const maximum_sql_offset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1);
    return jb::jobu::is_valid_attempt_number(request.attempt.attempt_number) && request.limit >= 1 &&
           request.limit <= maximum_output_slice_bytes && request.offset <= maximum_sql_offset;
}

auto persisted_output_error(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.invariant",
            .message  = "Persisted output metadata is invalid",
            .detail   = std::string{reason}};
}

auto observed_count(jb::core::JsonValue const& value) -> std::optional<std::uint64_t>
{
    if (value.is_uint()) {
        return value.as_uint();
    }
    if (value.is_int() && value.as_int() >= 0) {
        return static_cast<std::uint64_t>(value.as_int());
    }
    return std::nullopt;
}

auto channel_metadata_name(jb::jobu::OutputChannel channel) -> std::string_view
{
    switch (channel) {
        case jb::jobu::OutputChannel::Stdout:
            return "stdout";
        case jb::jobu::OutputChannel::Stderr:
            return "stderr";
        case jb::jobu::OutputChannel::Body:
            return "body";
        case jb::jobu::OutputChannel::Headers:
            return "headers";
    }
    return {};
}

struct CaptureEvidence {
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> captured_bytes;
    bool                         truncated{false};
    bool                         capture_lost{false};
};

auto capture_evidence(jb::jobu::detail::OutputRead const& read, jb::jobu::OutputChannel channel)
    -> ServiceResult<CaptureEvidence>
{
    auto evidence = CaptureEvidence{};
    if (!read.attempt.result) {
        return ServiceResult<CaptureEvidence>::success(evidence);
    }
    auto const& result = read.attempt.result->as_object();
    if (auto found = result.find("capture_lost"); found != result.end()) {
        if (!found->second.is_bool()) {
            return ServiceResult<CaptureEvidence>::failure(persisted_output_error("invalid_capture_loss"));
        }
        evidence.capture_lost = found->second.as_bool();
    }

    auto const expected_type = read.type == jb::jobu::JobType::Cli ? std::string_view{"cli"} : std::string_view{"http"};
    if (auto found = result.find("type");
        found != result.end() && (!found->second.is_string() || found->second.as_string() != expected_type)) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("mismatched_result_type"));
    }

    auto const name  = channel_metadata_name(channel);
    auto const found = result.find(name);
    if (found == result.end()) {
        return ServiceResult<CaptureEvidence>::success(evidence);
    }
    if (!found->second.is_object()) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("invalid_channel_metadata"));
    }
    auto const& object    = found->second.as_object();
    auto const  total     = object.find("total_bytes");
    auto const  captured  = object.find("captured_bytes");
    auto const  truncated = object.find("truncated");
    if (total == object.end() || captured == object.end() || truncated == object.end() ||
        !truncated->second.is_bool()) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("incomplete_channel_metadata"));
    }
    evidence.total_bytes    = observed_count(total->second);
    evidence.captured_bytes = observed_count(captured->second);
    if (!evidence.total_bytes || !evidence.captured_bytes || *evidence.captured_bytes > *evidence.total_bytes) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("invalid_channel_counts"));
    }
    evidence.truncated = truncated->second.as_bool();
    if (evidence.truncated != (*evidence.captured_bytes < *evidence.total_bytes)) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("inconsistent_truncation"));
    }
    if (read.channel_present && *evidence.captured_bytes != read.retained_bytes) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("retained_count_mismatch"));
    }
    if (read.channel_present && evidence.truncated != read.truncated) {
        return ServiceResult<CaptureEvidence>::failure(persisted_output_error("retained_truncation_mismatch"));
    }
    return ServiceResult<CaptureEvidence>::success(evidence);
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
        if (!is_valid_attempt_number(key.attempt_number)) {
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

auto HistoryService::read_output(AttemptOutputRequest const& request) -> ServiceResult<AttemptOutputChunk>
{
    auto* data = d_ptr<Private>();
    return data->invoke<AttemptOutputChunk>(*this, [&] {
        if (!valid_output_request(request)) {
            return ServiceResult<AttemptOutputChunk>::failure(invalid_request());
        }

        auto found = data->repository.read_output(request);
        if (!found) {
            return ServiceResult<AttemptOutputChunk>::failure(std::move(found).error());
        }
        if (!found->has_value()) {
            return ServiceResult<AttemptOutputChunk>::failure(
                error(jb::core::ErrorCategory::NotFound, "jobu.attempt.not_found", "Attempt was not found"));
        }
        auto& read     = **found;
        auto  evidence = capture_evidence(read, request.channel);
        if (!evidence) {
            return ServiceResult<AttemptOutputChunk>::failure(std::move(evidence).error());
        }

        auto chunk = AttemptOutputChunk{
            .attempt        = request.attempt,
            .channel        = request.channel,
            .offset         = request.offset,
            .retained_bytes = read.retained_bytes,
            .truncated      = read.truncated || evidence->truncated,
            .capture_lost   = read.capture_lost || evidence->capture_lost,
        };
        // Loss outranks a retained BLOB: recovery can persist an empty BLOB when capture is unknown.
        if (read.attempt.state != AttemptState::Completed) {
            chunk.status = OutputStatus::Pending;
        }
        else if (chunk.capture_lost) {
            chunk.status = OutputStatus::Lost;
        }
        else if (read.channel_present) {
            chunk.status = OutputStatus::Available;
        }
        else {
            chunk.status = OutputStatus::NotCaptured;
        }

        // Never turn a missing observation into a measured zero or a fabricated omitted-byte count.
        if (evidence->total_bytes && *evidence->total_bytes >= chunk.retained_bytes) {
            chunk.total_bytes   = evidence->total_bytes;
            chunk.omitted_bytes = *evidence->total_bytes - chunk.retained_bytes;
        }
        chunk.data           = std::move(read.bytes);
        chunk.bytes_returned = chunk.data.size();
        auto const end       = chunk.offset + chunk.bytes_returned;
        if (end < chunk.retained_bytes) {
            chunk.next_offset = end;
        }
        chunk.encoding =
            detail::is_valid_utf8(jb::core::as_string_view(chunk.data)) ? OutputEncoding::Utf8 : OutputEncoding::Base64;
        return ServiceResult<AttemptOutputChunk>::success(std::move(chunk));
    });
}

void HistoryService::shutdown() noexcept
{
    auto* data      = d_ptr<Private>();
    data->accepting = false;
    data->cursors.clear();
}

} // namespace jb::jobu
