#include "history_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "value.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {
namespace {

template <typename T>
using ReadResult = jb::core::Result<T, jb::core::Error>;

auto invariant(std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Internal,
            .code     = "jobu.storage.invariant",
            .message  = "Persisted output violates a JobU invariant",
            .detail   = std::string{reason}};
}

auto invalid_request() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.history.invalid_request",
            .message  = "History request is invalid"};
}

auto channel_column(OutputChannel channel) -> std::string_view
{
    switch (channel) {
        case OutputChannel::Stdout:
        case OutputChannel::Body:
            return "stdout_blob";
        case OutputChannel::Stderr:
        case OutputChannel::Headers:
            return "stderr_blob";
    }
    return {};
}

auto truncation_column(OutputChannel channel) -> std::string_view
{
    switch (channel) {
        case OutputChannel::Stdout:
        case OutputChannel::Body:
            return "stdout_truncated";
        case OutputChannel::Stderr:
        case OutputChannel::Headers:
            return "stderr_truncated";
    }
    return {};
}

auto read_length(jb::db::Record const& record) -> ReadResult<std::uint64_t>
{
    auto const* value  = record.value("channel_length");
    auto const* length = value ? std::get_if<std::int64_t>(value) : nullptr;
    if (!length || *length < 0) {
        return ReadResult<std::uint64_t>::failure(invariant("invalid_channel_length"));
    }
    return ReadResult<std::uint64_t>::success(static_cast<std::uint64_t>(*length));
}

} // namespace

auto HistoryRepository::read_output(AttemptOutputRequest const& request) -> ReadResult<std::optional<OutputRead>>
{
    if (request.limit < 1 || request.limit > 65'536 ||
        request.offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1)) {
        return ReadResult<std::optional<OutputRead>>::failure(invalid_request());
    }
    auto found = get_attempt(request.attempt);
    if (!found) {
        return ReadResult<std::optional<OutputRead>>::failure(std::move(found).error());
    }
    if (!found->has_value()) {
        return ReadResult<std::optional<OutputRead>>::success(std::nullopt);
    }

    auto number = attempt_number_to_storage(request.attempt.attempt_number);
    if (!number) {
        return ReadResult<std::optional<OutputRead>>::failure(std::move(number).error());
    }
    auto const column = channel_column(request.channel);
    auto const flag   = truncation_column(request.channel);
    if (column.empty() || flag.empty()) {
        return ReadResult<std::optional<OutputRead>>::failure(invalid_request());
    }

    auto read        = OutputRead{.attempt = std::move(**found)};
    auto row_present = false;
    {
        // typeof and length inspect the channel without asking the driver to materialize its bytes.
        auto sql = fmt::format(
            R"(SELECT r.type AS run_type,
                      o.run_id IS NOT NULL AS output_present,
                      typeof(o.{0}) AS channel_type,
                      length(o.{0}) AS channel_length,
                      o.{1} AS channel_truncated,
                      o.capture_lost AS capture_lost
               FROM jobu_runs r
               LEFT JOIN jobu_attempt_output o
                   ON o.run_id = r.id AND o.attempt_number = :number
               WHERE r.id = :run_id)",
            column,
            flag);
        auto query  = jb::db::Query{_database};
        auto result = query.prepare(sql);
        if (result) {
            result = query.bind_value(":number", *number);
        }
        if (result) {
            result = query.bind_value(":run_id", uuid_to_storage(request.attempt.run_id));
        }
        if (result) {
            result = query.exec();
        }
        if (!result) {
            return ReadResult<std::optional<OutputRead>>::failure(std::move(result).error());
        }
        auto next = query.next();
        if (!next) {
            return ReadResult<std::optional<OutputRead>>::failure(std::move(next).error());
        }
        if (!*next) {
            return ReadResult<std::optional<OutputRead>>::failure(invariant("attempt_without_run"));
        }

        auto type         = read_job_type(query.record(), "run_type");
        auto present      = read_boolean(query.record(), "output_present");
        auto storage_type = read_text(query.record(), "channel_type");
        if (!type) {
            return ReadResult<std::optional<OutputRead>>::failure(std::move(type).error());
        }
        if (!present) {
            return ReadResult<std::optional<OutputRead>>::failure(std::move(present).error());
        }
        if (!storage_type) {
            return ReadResult<std::optional<OutputRead>>::failure(std::move(storage_type).error());
        }
        read.type            = *type;
        row_present          = *present;
        read.channel_present = *storage_type == "blob";
        if (!read.channel_present && *storage_type != "null") {
            return ReadResult<std::optional<OutputRead>>::failure(invariant("channel_not_blob"));
        }
        if (row_present) {
            auto truncated = read_boolean(query.record(), "channel_truncated");
            auto lost      = read_boolean(query.record(), "capture_lost");
            if (!truncated) {
                return ReadResult<std::optional<OutputRead>>::failure(std::move(truncated).error());
            }
            if (!lost) {
                return ReadResult<std::optional<OutputRead>>::failure(std::move(lost).error());
            }
            read.truncated    = *truncated;
            read.capture_lost = *lost;
        }
        if (read.channel_present) {
            auto length = read_length(query.record());
            if (!length) {
                return ReadResult<std::optional<OutputRead>>::failure(std::move(length).error());
            }
            read.retained_bytes = *length;
        }
    }

    auto const cli_channel = request.channel == OutputChannel::Stdout || request.channel == OutputChannel::Stderr;
    if ((read.type == JobType::Cli) != cli_channel) {
        return ReadResult<std::optional<OutputRead>>::failure(invalid_request());
    }
    if (read.attempt.state != AttemptState::Completed && row_present) {
        return ReadResult<std::optional<OutputRead>>::failure(invariant("output_before_completion"));
    }

    if (request.offset > read.retained_bytes) {
        return ReadResult<std::optional<OutputRead>>::failure(invalid_request());
    }
    if (read.attempt.state != AttemptState::Completed || !read.channel_present ||
        request.offset == read.retained_bytes) {
        return ReadResult<std::optional<OutputRead>>::success(std::move(read));
    }

    // This second query projects at most the requested limit. The metadata cursor is already gone.
    auto sql = fmt::format(
        R"(SELECT substr({}, :start, :limit) AS channel_slice
           FROM jobu_attempt_output
           WHERE run_id = :run_id AND attempt_number = :number)",
        column);
    auto query  = jb::db::Query{_database};
    auto result = query.prepare(sql);
    if (result) {
        result = query.bind_value(":run_id", uuid_to_storage(request.attempt.run_id));
    }
    if (result) {
        result = query.bind_value(":number", *number);
    }
    if (result) {
        result = query.bind_value(":start", static_cast<std::int64_t>(request.offset + 1U));
    }
    if (result) {
        result = query.bind_value(":limit", static_cast<std::int64_t>(request.limit));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return ReadResult<std::optional<OutputRead>>::failure(std::move(result).error());
    }
    auto next = query.next();
    if (!next) {
        return ReadResult<std::optional<OutputRead>>::failure(std::move(next).error());
    }
    if (!*next) {
        return ReadResult<std::optional<OutputRead>>::failure(invariant("missing_output_slice"));
    }
    auto const* value = query.value("channel_slice");
    auto const* bytes = value ? std::get_if<jb::core::ByteBuffer>(value) : nullptr;
    auto const  expected =
        static_cast<std::size_t>(std::min<std::uint64_t>(request.limit, read.retained_bytes - request.offset));
    if (!bytes || bytes->size() != expected) {
        return ReadResult<std::optional<OutputRead>>::failure(invariant("invalid_output_slice"));
    }
    read.bytes = *bytes;
    return ReadResult<std::optional<OutputRead>>::success(std::move(read));
}

} // namespace jb::jobu::detail
