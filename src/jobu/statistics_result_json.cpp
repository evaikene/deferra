#include "statistics_json.hpp"

#include "history_scalar_codec_priv.hpp"
#include "json.hpp"
#include "utc_timestamp.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {
namespace {

using jb::core::JsonValue;
template <typename T>
using CodecResult = jb::core::Result<T, jb::core::Error>;

auto invalid() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.protocol.invalid_response",
            .message  = "The JobU statistics response is invalid"};
}

template <typename T>
auto reject() -> CodecResult<T>
{
    return CodecResult<T>::failure(invalid());
}

auto json(auto value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto member(JsonValue::Object const& object, std::string_view name) -> JsonValue const*
{
    auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

auto unsigned_value(JsonValue const& value, std::uint64_t& number) -> bool
{
    if (value.is_uint()) {
        number = value.as_uint();
        return true;
    }
    if (value.is_int() && value.as_int() >= 0) {
        number = static_cast<std::uint64_t>(value.as_int());
        return true;
    }
    return false;
}

auto nonnegative_number(JsonValue const& value, double& number) -> bool
{
    if (value.is_double()) {
        number = value.as_double();
    }
    else if (value.is_uint() && value.as_uint() <= (std::uint64_t{1} << 53U)) {
        number = static_cast<double>(value.as_uint());
    }
    else if (value.is_int() && value.as_int() >= 0 && value.as_int() <= (std::int64_t{1} << 53U)) {
        number = static_cast<double>(value.as_int());
    }
    else {
        return false;
    }
    return std::isfinite(number) && number >= 0.0;
}

template <typename Counts>
struct NamedCount {
    std::string_view name;
    std::uint64_t Counts::* field;
};

constexpr std::array run_states{
    NamedCount<StatisticsRunCounts>{.name = "scheduled",   .field = &StatisticsRunCounts::scheduled  },
    NamedCount<StatisticsRunCounts>{.name = "running",     .field = &StatisticsRunCounts::running    },
    NamedCount<StatisticsRunCounts>{.name = "retry_wait",  .field = &StatisticsRunCounts::retry_wait },
    NamedCount<StatisticsRunCounts>{.name = "succeeded",   .field = &StatisticsRunCounts::succeeded  },
    NamedCount<StatisticsRunCounts>{.name = "failed",      .field = &StatisticsRunCounts::failed     },
    NamedCount<StatisticsRunCounts>{.name = "interrupted", .field = &StatisticsRunCounts::interrupted},
    NamedCount<StatisticsRunCounts>{.name = "cancelled",   .field = &StatisticsRunCounts::cancelled  },
};
constexpr std::array run_types{
    NamedCount<StatisticsRunCounts>{.name = "cli",  .field = &StatisticsRunCounts::cli },
    NamedCount<StatisticsRunCounts>{.name = "http", .field = &StatisticsRunCounts::http},
};
constexpr std::array run_origins{
    NamedCount<StatisticsRunCounts>{.name = "scheduled", .field = &StatisticsRunCounts::scheduled_origin},
    NamedCount<StatisticsRunCounts>{.name = "manual",    .field = &StatisticsRunCounts::manual_origin   },
};
constexpr std::array attempt_states{
    NamedCount<StatisticsAttemptCounts>{.name = "pending",   .field = &StatisticsAttemptCounts::pending  },
    NamedCount<StatisticsAttemptCounts>{.name = "running",   .field = &StatisticsAttemptCounts::running  },
    NamedCount<StatisticsAttemptCounts>{.name = "completed", .field = &StatisticsAttemptCounts::completed},
};
constexpr std::array attempt_outcomes{
    NamedCount<StatisticsAttemptCounts>{.name = "succeeded",   .field = &StatisticsAttemptCounts::succeeded  },
    NamedCount<StatisticsAttemptCounts>{.name = "failed",      .field = &StatisticsAttemptCounts::failed     },
    NamedCount<StatisticsAttemptCounts>{.name = "interrupted", .field = &StatisticsAttemptCounts::interrupted},
    NamedCount<StatisticsAttemptCounts>{.name = "cancelled",   .field = &StatisticsAttemptCounts::cancelled  },
};
constexpr std::array capture_counts{
    NamedCount<StatisticsCaptureCounts>{.name  = "truncated_attempts",
                                        .field = &StatisticsCaptureCounts::truncated_attempts                          },
    NamedCount<StatisticsCaptureCounts>{.name = "lost_attempts",       .field = &StatisticsCaptureCounts::lost_attempts},
};

template <typename Counts, std::size_t N>
auto encode_counts(Counts const& counts, std::array<NamedCount<Counts>, N> const& fields) -> JsonValue
{
    auto object = JsonValue::Object{};
    for (auto const& field : fields) {
        object.emplace(std::string{field.name}, json(counts.*(field.field)));
    }
    return json(std::move(object));
}

template <typename Counts, std::size_t N>
auto decode_counts(JsonValue const& value, Counts& counts, std::array<NamedCount<Counts>, N> const& fields) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    for (auto const& field : fields) {
        auto const* number = member(value.as_object(), field.name);
        if (!number || !unsigned_value(*number, counts.*(field.field))) {
            return false;
        }
    }
    return true;
}

auto encode_run_counts(StatisticsRunCounts const& counts) -> JsonValue
{
    return json(JsonValue::Object{
        {"total", json(counts.total)},
        {"states", encode_counts(counts, run_states)},
        {"types", encode_counts(counts, run_types)},
        {"origins", encode_counts(counts, run_origins)},
    });
}

auto decode_run_counts(JsonValue const& value, StatisticsRunCounts& counts) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    auto const& fields  = value.as_object();
    auto const* total   = member(fields, "total");
    auto const* states  = member(fields, "states");
    auto const* types   = member(fields, "types");
    auto const* origins = member(fields, "origins");
    return total != nullptr && unsigned_value(*total, counts.total) && states != nullptr &&
           decode_counts(*states, counts, run_states) && types != nullptr && decode_counts(*types, counts, run_types) &&
           origins != nullptr && decode_counts(*origins, counts, run_origins);
}

auto encode_attempt_counts(StatisticsAttemptCounts const& counts) -> JsonValue
{
    return json(JsonValue::Object{
        {"total", json(counts.total)},
        {"states", encode_counts(counts, attempt_states)},
        {"outcomes", encode_counts(counts, attempt_outcomes)},
        {"retries", json(counts.retries)},
    });
}

auto decode_attempt_counts(JsonValue const& value, StatisticsAttemptCounts& counts) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    auto const& fields   = value.as_object();
    auto const* total    = member(fields, "total");
    auto const* states   = member(fields, "states");
    auto const* outcomes = member(fields, "outcomes");
    auto const* retries  = member(fields, "retries");
    return total != nullptr && unsigned_value(*total, counts.total) && states != nullptr &&
           decode_counts(*states, counts, attempt_states) && outcomes != nullptr &&
           decode_counts(*outcomes, counts, attempt_outcomes) && retries != nullptr &&
           unsigned_value(*retries, counts.retries);
}

auto encode_duration(StatisticsDuration const& duration) -> std::optional<JsonValue>
{
    if ((duration.samples == 0 && (duration.average || duration.maximum)) ||
        (duration.samples != 0 && (!duration.average || !duration.maximum)) ||
        (duration.average && (!std::isfinite(*duration.average) || *duration.average < 0.0)) ||
        (duration.maximum && (!std::isfinite(*duration.maximum) || *duration.maximum < 0.0))) {
        return std::nullopt;
    }
    return json(JsonValue::Object{
        {"samples", json(duration.samples)                                                 },
        {"average", duration.average ? json(*duration.average) : json(jb::core::JsonNull{})},
        {"maximum", duration.maximum ? json(*duration.maximum) : json(jb::core::JsonNull{})},
    });
}

auto decode_duration(JsonValue const& value, StatisticsDuration& duration) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    auto const& fields  = value.as_object();
    auto const* samples = member(fields, "samples");
    auto const* average = member(fields, "average");
    auto const* maximum = member(fields, "maximum");
    if (!samples || !unsigned_value(*samples, duration.samples) || !average || !maximum) {
        return false;
    }
    if (!average->is_null()) {
        auto number = double{};
        if (!nonnegative_number(*average, number)) {
            return false;
        }
        duration.average = number;
    }
    if (!maximum->is_null()) {
        auto number = double{};
        if (!nonnegative_number(*maximum, number)) {
            return false;
        }
        duration.maximum = number;
    }
    return (duration.samples == 0 && !duration.average && !duration.maximum) ||
           (duration.samples != 0 && duration.average && duration.maximum);
}

auto group_text(StatisticsGroupBy group_by) -> std::optional<std::string_view>
{
    switch (group_by) {
        case StatisticsGroupBy::None:
            return "none";
        case StatisticsGroupBy::Queue:
            return "queue";
        case StatisticsGroupBy::Job:
            return "job";
        case StatisticsGroupBy::Type:
            return "type";
        case StatisticsGroupBy::Origin:
            return "origin";
        case StatisticsGroupBy::State:
            return "state";
    }
    return std::nullopt;
}

auto parse_group(std::string_view text, StatisticsGroupBy& group_by) -> bool
{
    for (auto candidate : {StatisticsGroupBy::None,
                           StatisticsGroupBy::Queue,
                           StatisticsGroupBy::Job,
                           StatisticsGroupBy::Type,
                           StatisticsGroupBy::Origin,
                           StatisticsGroupBy::State}) {
        if (group_text(candidate) == text) {
            group_by = candidate;
            return true;
        }
    }
    return false;
}

auto encode_key(StatisticsGroupKey const& key, StatisticsGroupBy group_by) -> std::optional<JsonValue>
{
    switch (group_by) {
        case StatisticsGroupBy::None:
            if (std::holds_alternative<std::monostate>(key)) {
                return json(jb::core::JsonNull{});
            }
            break;
        case StatisticsGroupBy::Queue:
        case StatisticsGroupBy::Job:
            if (auto const* id = std::get_if<jb::core::Uuid>(&key); id && !id->is_nil()) {
                return json(id->to_string());
            }
            break;
        case StatisticsGroupBy::Type:
            if (auto const* type = std::get_if<JobType>(&key)) {
                if (auto text = detail::history_wire_text(*type)) {
                    return json(std::string{*text});
                }
            }
            break;
        case StatisticsGroupBy::Origin:
            if (auto const* origin = std::get_if<RunOrigin>(&key)) {
                if (auto text = detail::history_wire_text(*origin)) {
                    return json(std::string{*text});
                }
            }
            break;
        case StatisticsGroupBy::State:
            if (auto const* state = std::get_if<RunState>(&key)) {
                if (auto text = detail::history_wire_text(*state)) {
                    return json(std::string{*text});
                }
            }
            break;
    }
    return std::nullopt;
}

auto decode_key(JsonValue const& value, StatisticsGroupBy group_by, StatisticsGroupKey& key) -> bool
{
    switch (group_by) {
        case StatisticsGroupBy::None:
            return value.is_null();
        case StatisticsGroupBy::Queue:
        case StatisticsGroupBy::Job:
            if (value.is_string()) {
                auto id = jb::core::Uuid::parse(value.as_string());
                if (id && !id->is_nil() && id->to_string() == value.as_string()) {
                    key = std::move(id).value();
                    return true;
                }
            }
            return false;
        case StatisticsGroupBy::Type:
            if (value.is_string()) {
                auto type = JobType{};
                if (detail::parse_history_wire_text(value.as_string(), type)) {
                    key = type;
                    return true;
                }
            }
            return false;
        case StatisticsGroupBy::Origin:
            if (value.is_string()) {
                auto origin = RunOrigin{};
                if (detail::parse_history_wire_text(value.as_string(), origin)) {
                    key = origin;
                    return true;
                }
            }
            return false;
        case StatisticsGroupBy::State:
            if (value.is_string()) {
                auto state = RunState{};
                if (detail::parse_history_wire_text(value.as_string(), state)) {
                    key = state;
                    return true;
                }
            }
            return false;
    }
    return false;
}

auto encode_group(StatisticsGroup const& group, StatisticsGroupBy group_by) -> std::optional<JsonValue>
{
    auto key       = encode_key(group.key, group_by);
    auto lateness  = encode_duration(group.schedule_lateness_ms);
    auto execution = encode_duration(group.execution_wall_duration_ms);
    auto wait      = group.runnable_wait_ms ? encode_duration(*group.runnable_wait_ms)
                                            : std::optional<JsonValue>{json(jb::core::JsonNull{})};
    if (!key || !lateness || !execution || !wait) {
        return std::nullopt;
    }
    return json(JsonValue::Object{
        {"key", std::move(*key)},
        {"runs", encode_run_counts(group.runs)},
        {"attempts", encode_attempt_counts(group.attempts)},
        {"capture", encode_counts(group.capture, capture_counts)},
        {"schedule_lateness_ms", std::move(*lateness)},
        {"execution_wall_duration_ms", std::move(*execution)},
        {"runnable_wait_ms", std::move(*wait)},
    });
}

auto decode_group(JsonValue const& value, StatisticsGroupBy group_by) -> std::optional<StatisticsGroup>
{
    if (!value.is_object()) {
        return std::nullopt;
    }
    auto const& fields    = value.as_object();
    auto const* key       = member(fields, "key");
    auto const* runs      = member(fields, "runs");
    auto const* attempts  = member(fields, "attempts");
    auto const* capture   = member(fields, "capture");
    auto const* lateness  = member(fields, "schedule_lateness_ms");
    auto const* execution = member(fields, "execution_wall_duration_ms");
    auto const* wait      = member(fields, "runnable_wait_ms");
    if (!key || !runs || !attempts || !capture || !lateness || !execution || !wait) {
        return std::nullopt;
    }

    auto group = StatisticsGroup{};
    if (!decode_key(*key, group_by, group.key) || !decode_run_counts(*runs, group.runs) ||
        !decode_attempt_counts(*attempts, group.attempts) || !decode_counts(*capture, group.capture, capture_counts)) {
        return std::nullopt;
    }

    if (!decode_duration(*lateness, group.schedule_lateness_ms) ||
        !decode_duration(*execution, group.execution_wall_duration_ms)) {
        return std::nullopt;
    }
    if (!wait->is_null()) {
        group.runnable_wait_ms.emplace();
        if (!decode_duration(*wait, *group.runnable_wait_ms)) {
            return std::nullopt;
        }
    }
    return group;
}

auto encode_window(UtcRange const& window) -> std::optional<JsonValue>
{
    if (!window.from || !window.to || *window.from >= *window.to) {
        return std::nullopt;
    }
    auto from = format_utc_timestamp(*window.from);
    auto to   = format_utc_timestamp(*window.to);
    if (!from || !to) {
        return std::nullopt;
    }
    return json(JsonValue::Object{
        {"from", json(std::move(from).value())},
        {"to",   json(std::move(to).value())  }
    });
}

auto decode_window(JsonValue const& value, UtcRange& window) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    auto const* from = member(value.as_object(), "from");
    auto const* to   = member(value.as_object(), "to");
    if (!from || !from->is_string() || !to || !to->is_string()) {
        return false;
    }
    auto lower = parse_utc_timestamp(from->as_string());
    auto upper = parse_utc_timestamp(to->as_string());
    if (!lower || !upper || *lower >= *upper) {
        return false;
    }
    window = {.from = *lower, .to = *upper};
    return true;
}

auto encode_measurement(StatisticsMeasurement const& measurement) -> JsonValue
{
    return json(JsonValue::Object{
        {"timing",        json(measurement.timing)       },
        {"runnable_wait", json(measurement.runnable_wait)},
        {"capture",       json(measurement.capture)      },
    });
}

auto decode_measurement(JsonValue const& value, StatisticsMeasurement& measurement) -> bool
{
    if (!value.is_object()) {
        return false;
    }
    auto const& fields  = value.as_object();
    auto const* timing  = member(fields, "timing");
    auto const* wait    = member(fields, "runnable_wait");
    auto const* capture = member(fields, "capture");
    if (!timing || !timing->is_string() || !wait || !wait->is_string() || !capture || !capture->is_string()) {
        return false;
    }
    measurement.timing        = timing->as_string();
    measurement.runnable_wait = wait->as_string();
    measurement.capture       = capture->as_string();
    return true;
}

} // namespace

auto statistics_page_to_json(StatisticsPage const& page) -> CodecResult<JsonValue>
{
    auto window   = encode_window(page.window);
    auto group_by = group_text(page.group_by);
    if (!window || !group_by || page.groups.size() > 200U ||
        (page.next_cursor && (page.next_cursor->empty() || page.groups.empty())) ||
        (page.group_by == StatisticsGroupBy::None && (page.groups.size() != 1U || page.next_cursor))) {
        return reject<JsonValue>();
    }
    auto groups = JsonValue::Array{};
    groups.reserve(page.groups.size());
    for (auto const& group : page.groups) {
        auto encoded = encode_group(group, page.group_by);
        if (!encoded) {
            return reject<JsonValue>();
        }
        groups.push_back(std::move(*encoded));
    }
    auto result = json(JsonValue::Object{
        {"window",      std::move(*window)                                                     },
        {"group_by",    json(std::string{*group_by})                                           },
        {"groups",      json(std::move(groups))                                                },
        {"next_cursor", page.next_cursor ? json(*page.next_cursor) : json(jb::core::JsonNull{})},
        {"measurement", encode_measurement(page.measurement)                                   },
    });
    if (!jb::core::serialize_json(result)) {
        return reject<JsonValue>();
    }
    return CodecResult<JsonValue>::success(std::move(result));
}

auto statistics_page_from_json(JsonValue const& value) -> CodecResult<StatisticsPage>
{
    if (!value.is_object()) {
        return reject<StatisticsPage>();
    }
    auto const& fields      = value.as_object();
    auto const* window      = member(fields, "window");
    auto const* group_by    = member(fields, "group_by");
    auto const* groups      = member(fields, "groups");
    auto const* cursor      = member(fields, "next_cursor");
    auto const* measurement = member(fields, "measurement");
    if (!window || !group_by || !groups || !cursor || !measurement || !groups->is_array() ||
        groups->as_array().size() > 200U ||
        (!cursor->is_null() && (!cursor->is_string() || cursor->as_string().empty())) ||
        (groups->as_array().empty() && !cursor->is_null())) {
        return reject<StatisticsPage>();
    }

    auto page = StatisticsPage{};
    if (!decode_window(*window, page.window) || !group_by->is_string() ||
        !parse_group(group_by->as_string(), page.group_by) || !decode_measurement(*measurement, page.measurement)) {
        return reject<StatisticsPage>();
    }
    if (page.group_by == StatisticsGroupBy::None && (groups->as_array().size() != 1U || !cursor->is_null())) {
        return reject<StatisticsPage>();
    }

    // The grouping discriminator fixes the type of every key on this page.
    page.groups.reserve(groups->as_array().size());
    for (auto const& value : groups->as_array()) {
        auto decoded = decode_group(value, page.group_by);
        if (!decoded) {
            return reject<StatisticsPage>();
        }
        page.groups.push_back(*decoded);
    }
    if (cursor->is_string()) {
        page.next_cursor = cursor->as_string();
    }
    return CodecResult<StatisticsPage>::success(std::move(page));
}

} // namespace jb::jobu
