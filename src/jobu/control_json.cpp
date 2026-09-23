#include "control_json.hpp"

#include "history_json.hpp"
#include "utc_timestamp.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {

namespace {

using jb::core::Error;
using jb::core::ErrorCategory;
using jb::core::JsonValue;

template <typename T>
using ConversionResult = jb::core::Result<T, Error>;

auto invalid(bool request) -> Error
{
    return {.category = ErrorCategory::InvalidArgument,
            .code     = request ? "jobu.protocol.invalid_request" : "jobu.protocol.invalid_response",
            .message  = request ? "The JobU control request is invalid" : "The JobU control response is invalid"};
}

template <typename T>
auto reject(bool request) -> ConversionResult<T>
{
    return ConversionResult<T>::failure(invalid(request));
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

auto only_members(JsonValue::Object const& object, std::initializer_list<std::string_view> allowed) -> bool
{
    for (auto const& [name, value] : object) {
        static_cast<void>(value);
        auto permitted = false;
        for (auto candidate : allowed) {
            if (name == candidate) {
                permitted = true;
                break;
            }
        }
        if (!permitted) {
            return false;
        }
    }
    return true;
}

auto checked_request(JsonValue value) -> ConversionResult<JsonValue>
{
    if (!jb::core::serialize_json(value)) {
        return reject<JsonValue>(true);
    }
    return ConversionResult<JsonValue>::success(std::move(value));
}

auto decode_uuid(JsonValue const& value, jb::core::Uuid& result) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = jb::core::Uuid::parse(value.as_string());
    if (!parsed || parsed->to_string() != value.as_string()) {
        return false;
    }
    result = std::move(parsed).value();
    return true;
}

auto decode_count(JsonValue const& value, std::size_t& result) -> bool
{
    auto decoded = std::uint64_t{};
    if (value.is_uint()) {
        decoded = value.as_uint();
    }
    else if (value.is_int() && value.as_int() >= 0) {
        decoded = static_cast<std::uint64_t>(value.as_int());
    }
    else {
        return false;
    }
    if (decoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    result = static_cast<std::size_t>(decoded);
    return true;
}

auto cron_schedule_from_json(JsonValue const& value) -> ConversionResult<CronSchedule>
{
    if (!value.is_object() || !only_members(value.as_object(), {"kind", "expression", "timezone"})) {
        return reject<CronSchedule>(true);
    }
    auto const& object     = value.as_object();
    auto const* kind       = member(object, "kind");
    auto const* expression = member(object, "expression");
    auto const* timezone   = member(object, "timezone");
    if (!kind || !kind->is_string() || kind->as_string() != "cron" || !expression || !expression->is_string() ||
        !timezone || !timezone->is_string()) {
        return reject<CronSchedule>(true);
    }
    return ConversionResult<CronSchedule>::success(
        {.expression = expression->as_string(), .timezone = timezone->as_string()});
}

auto cron_schedule_to_json(CronSchedule const& schedule) -> JsonValue
{
    return json(JsonValue::Object{
        {"kind",       json(std::string{"cron"})},
        {"expression", json(schedule.expression)},
        {"timezone",   json(schedule.timezone)  },
    });
}

} // namespace

auto run_now_request_to_json(RunNowRequest const& request) -> ConversionResult<JsonValue>
{
    auto object = JsonValue::Object{
        {"job_id", json(request.job_id.to_string())},
    };
    if (request.idempotency_key) {
        object.emplace("idempotency_key", json(*request.idempotency_key));
    }
    return checked_request(json(std::move(object)));
}

auto run_now_request_from_json(JsonValue const& value) -> ConversionResult<RunNowRequest>
{
    if (!value.is_object() || !only_members(value.as_object(), {"job_id", "idempotency_key"})) {
        return reject<RunNowRequest>(true);
    }
    auto const& object  = value.as_object();
    auto const* job_id  = member(object, "job_id");
    auto        request = RunNowRequest{};
    if (!job_id || !decode_uuid(*job_id, request.job_id)) {
        return reject<RunNowRequest>(true);
    }
    if (auto const* key = member(object, "idempotency_key")) {
        if (!key->is_string()) {
            return reject<RunNowRequest>(true);
        }
        request.idempotency_key = key->as_string();
    }
    return ConversionResult<RunNowRequest>::success(std::move(request));
}

auto cancel_run_request_to_json(jb::core::Uuid const& run_id) -> ConversionResult<JsonValue>
{
    return checked_request(json(JsonValue::Object{
        {"run_id", json(run_id.to_string())},
    }));
}

auto cancel_run_request_from_json(JsonValue const& value) -> ConversionResult<jb::core::Uuid>
{
    if (!value.is_object() || !only_members(value.as_object(), {"run_id"})) {
        return reject<jb::core::Uuid>(true);
    }
    auto const* encoded = member(value.as_object(), "run_id");
    auto        run_id  = jb::core::Uuid{};
    if (!encoded || !decode_uuid(*encoded, run_id)) {
        return reject<jb::core::Uuid>(true);
    }
    return ConversionResult<jb::core::Uuid>::success(run_id);
}

auto cancel_run_result_to_json(CancelRunResult const& result, AttributeRegistry const& registry)
    -> ConversionResult<JsonValue>
{
    auto run = run_details_to_json(result.run, registry);
    if (!run) {
        return reject<JsonValue>(false);
    }
    auto disposition = std::string{};
    switch (result.disposition) {
        case CancelDisposition::Completed:
            disposition = "completed";
            break;
        case CancelDisposition::Requested:
            disposition = "requested";
            break;
        default:
            return reject<JsonValue>(false);
    }
    return ConversionResult<JsonValue>::success(json(JsonValue::Object{
        {"disposition", json(std::move(disposition))},
        {"run",         std::move(run).value()      },
    }));
}

auto cancel_run_result_from_json(JsonValue const& value, AttributeRegistry const& registry)
    -> ConversionResult<CancelRunResult>
{
    if (!value.is_object()) {
        return reject<CancelRunResult>(false);
    }
    auto const* disposition = member(value.as_object(), "disposition");
    auto const* run         = member(value.as_object(), "run");
    if (!disposition || !disposition->is_string() || !run) {
        return reject<CancelRunResult>(false);
    }
    auto decoded = run_details_from_json(*run, registry);
    if (!decoded) {
        return reject<CancelRunResult>(false);
    }
    auto result = CancelRunResult{};
    if (disposition->as_string() == "completed") {
        result.disposition = CancelDisposition::Completed;
    }
    else if (disposition->as_string() == "requested") {
        result.disposition = CancelDisposition::Requested;
    }
    else {
        return reject<CancelRunResult>(false);
    }
    auto const& details = *decoded;
    result.run          = {
        .id             = details.id,
        .job_id         = details.job_id,
        .job_revision   = details.job_revision,
        .queue_id       = details.queue_id,
        .origin         = details.origin,
        .schedule_owned = details.schedule_owned,
        .planned_at     = details.planned_at,
        .runnable_at    = details.runnable_at,
        .started_at     = details.started_at,
        .completed_at   = details.completed_at,
        .type           = details.type,
        .priority       = details.priority,
        .attributes     = details.attributes,
        .payload        = details.payload,
        .state          = details.state,
        .result         = details.result,
    };
    return ConversionResult<CancelRunResult>::success(std::move(result));
}

auto schedule_validate_request_to_json(CronSchedule const& schedule) -> ConversionResult<JsonValue>
{
    return checked_request(json(JsonValue::Object{
        {"schedule", cron_schedule_to_json(schedule)},
    }));
}

auto schedule_validate_request_from_json(JsonValue const& value) -> ConversionResult<CronSchedule>
{
    if (!value.is_object() || !only_members(value.as_object(), {"schedule"})) {
        return reject<CronSchedule>(true);
    }
    auto const* schedule = member(value.as_object(), "schedule");
    return schedule ? cron_schedule_from_json(*schedule) : reject<CronSchedule>(true);
}

auto schedule_validate_result_to_json() -> JsonValue
{
    return json(JsonValue::Object{
        {"valid", json(true)},
    });
}

auto schedule_validate_result_from_json(JsonValue const& value) -> ConversionResult<void>
{
    if (!value.is_object()) {
        return reject<void>(false);
    }
    auto const* valid = member(value.as_object(), "valid");
    if (!valid || !valid->is_bool() || !valid->as_bool()) {
        return reject<void>(false);
    }
    return ConversionResult<void>::success();
}

auto schedule_next_request_to_json(ScheduleNextRequest const& request) -> ConversionResult<JsonValue>
{
    auto after = format_utc_timestamp(request.after);
    if (!after) {
        return reject<JsonValue>(true);
    }
    return checked_request(json(JsonValue::Object{
        {"schedule", cron_schedule_to_json(request.schedule)        },
        {"after",    json(std::move(after).value())                 },
        {"count",    json(static_cast<std::uint64_t>(request.count))},
    }));
}

auto schedule_next_request_from_json(JsonValue const& value) -> ConversionResult<ScheduleNextRequest>
{
    if (!value.is_object() || !only_members(value.as_object(), {"schedule", "after", "count"})) {
        return reject<ScheduleNextRequest>(true);
    }
    auto const& object   = value.as_object();
    auto const* schedule = member(object, "schedule");
    auto const* after    = member(object, "after");
    if (!schedule || !after || !after->is_string()) {
        return reject<ScheduleNextRequest>(true);
    }
    auto decoded_schedule = cron_schedule_from_json(*schedule);
    auto decoded_after    = parse_utc_timestamp(after->as_string());
    if (!decoded_schedule || !decoded_after) {
        return reject<ScheduleNextRequest>(true);
    }
    auto request =
        ScheduleNextRequest{.schedule = std::move(decoded_schedule).value(), .after = std::move(decoded_after).value()};
    if (auto const* count = member(object, "count"); count && !decode_count(*count, request.count)) {
        return reject<ScheduleNextRequest>(true);
    }
    return ConversionResult<ScheduleNextRequest>::success(std::move(request));
}

auto schedule_next_result_to_json(std::vector<jb::core::UtcTimePoint> const& occurrences) -> ConversionResult<JsonValue>
{
    if (occurrences.empty() || occurrences.size() > 200) {
        return reject<JsonValue>(false);
    }
    auto encoded = JsonValue::Array{};
    encoded.reserve(occurrences.size());
    for (auto index = std::size_t{0}; index < occurrences.size(); ++index) {
        if (index > 0 && occurrences[index] <= occurrences[index - 1]) {
            return reject<JsonValue>(false);
        }
        auto formatted = format_utc_timestamp(occurrences[index]);
        if (!formatted) {
            return reject<JsonValue>(false);
        }
        encoded.push_back(json(std::move(formatted).value()));
    }
    return ConversionResult<JsonValue>::success(json(JsonValue::Object{
        {"occurrences", json(std::move(encoded))},
    }));
}

auto schedule_next_result_from_json(JsonValue const& value) -> ConversionResult<std::vector<jb::core::UtcTimePoint>>
{
    if (!value.is_object()) {
        return reject<std::vector<jb::core::UtcTimePoint>>(false);
    }
    auto const* occurrences = member(value.as_object(), "occurrences");
    if (!occurrences || !occurrences->is_array() || occurrences->as_array().empty() ||
        occurrences->as_array().size() > 200) {
        return reject<std::vector<jb::core::UtcTimePoint>>(false);
    }
    auto decoded = std::vector<jb::core::UtcTimePoint>{};
    decoded.reserve(occurrences->as_array().size());
    for (auto const& item : occurrences->as_array()) {
        if (!item.is_string()) {
            return reject<std::vector<jb::core::UtcTimePoint>>(false);
        }
        auto timestamp = parse_utc_timestamp(item.as_string());
        if (!timestamp || (!decoded.empty() && *timestamp <= decoded.back())) {
            return reject<std::vector<jb::core::UtcTimePoint>>(false);
        }
        decoded.push_back(*timestamp);
    }
    return ConversionResult<std::vector<jb::core::UtcTimePoint>>::success(std::move(decoded));
}

} // namespace jb::jobu
