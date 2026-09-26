#include "history_json.hpp"

#include "attribute_registry.hpp"
#include "history_scalar_codec_priv.hpp"
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
            .message  = request ? "The JobU history request is invalid" : "The JobU history response is invalid"};
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

auto decode_uint(JsonValue const& value, std::uint64_t& result) -> bool
{
    if (value.is_uint()) {
        result = value.as_uint();
        return true;
    }
    if (value.is_int() && value.as_int() >= 0) {
        result = static_cast<std::uint64_t>(value.as_int());
        return true;
    }
    return false;
}

auto decode_i32(JsonValue const& value, std::int32_t& result) -> bool
{
    auto decoded = std::int64_t{};
    if (value.is_int()) {
        decoded = value.as_int();
    }
    else if (value.is_uint() &&
             value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        decoded = static_cast<std::int64_t>(value.as_uint());
    }
    else {
        return false;
    }
    if (decoded < std::numeric_limits<std::int32_t>::min() || decoded > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    result = static_cast<std::int32_t>(decoded);
    return true;
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

auto decode_time(JsonValue const& value, jb::core::UtcTimePoint& result) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = parse_utc_timestamp(value.as_string());
    if (!parsed) {
        return false;
    }
    result = std::move(parsed).value();
    return true;
}

auto decode_nullable_time(JsonValue const& value, std::optional<jb::core::UtcTimePoint>& result) -> bool
{
    if (value.is_null()) {
        result.reset();
        return true;
    }
    auto decoded = jb::core::UtcTimePoint{};
    if (!decode_time(value, decoded)) {
        return false;
    }
    result = decoded;
    return true;
}

auto encode_time(jb::core::UtcTimePoint value) -> std::optional<JsonValue>
{
    auto formatted = format_utc_timestamp(value);
    if (!formatted) {
        return std::nullopt;
    }
    return json(std::move(formatted).value());
}

auto encode_nullable_time(std::optional<jb::core::UtcTimePoint> value) -> std::optional<JsonValue>
{
    return value ? encode_time(*value) : std::optional<JsonValue>{json(jb::core::JsonNull{})};
}

template <typename Enum>
auto decode_enum(JsonValue const& value, Enum& result) -> bool
{
    return value.is_string() && detail::parse_history_wire_text(value.as_string(), result);
}

auto valid_range(UtcRange const& range) -> bool
{
    return !range.from || !range.to || *range.from < *range.to;
}

auto decode_range(JsonValue const& value, UtcRange& result) -> bool
{
    if (!value.is_object() || !only_members(value.as_object(), {"from", "to"})) {
        return false;
    }
    if (auto const* from = member(value.as_object(), "from")) {
        auto decoded = jb::core::UtcTimePoint{};
        if (!decode_time(*from, decoded)) {
            return false;
        }
        result.from = decoded;
    }
    if (auto const* to = member(value.as_object(), "to")) {
        auto decoded = jb::core::UtcTimePoint{};
        if (!decode_time(*to, decoded)) {
            return false;
        }
        result.to = decoded;
    }
    return valid_range(result);
}

auto append_range(JsonValue::Object& object, std::string_view name, UtcRange const& range) -> bool
{
    if (!valid_range(range)) {
        return false;
    }
    if (!range.from && !range.to) {
        return true;
    }
    auto bounds = JsonValue::Object{};
    if (range.from) {
        auto encoded = encode_time(*range.from);
        if (!encoded) {
            return false;
        }
        bounds.emplace("from", std::move(*encoded));
    }
    if (range.to) {
        auto encoded = encode_time(*range.to);
        if (!encoded) {
            return false;
        }
        bounds.emplace("to", std::move(*encoded));
    }
    object.emplace(name, json(std::move(bounds)));
    return true;
}

auto valid_limit(std::size_t limit) -> bool
{
    return limit >= 1 && limit <= 200;
}

auto decode_limit(JsonValue const& value, std::size_t& limit) -> bool
{
    auto decoded = std::uint64_t{};
    if (!decode_uint(value, decoded) || decoded < 1 || decoded > 200) {
        return false;
    }
    limit = static_cast<std::size_t>(decoded);
    return true;
}

auto valid_query(RunQuery const& query) -> bool
{
    return valid_limit(query.limit) && valid_range(query.filters.planned) && valid_range(query.filters.started) &&
           valid_range(query.filters.completed) &&
           (!query.filters.state || detail::history_wire_text(*query.filters.state)) &&
           (!query.filters.origin || detail::history_wire_text(*query.filters.origin)) &&
           (!query.filters.type || detail::history_wire_text(*query.filters.type));
}

auto append_enum(JsonValue::Object& object, std::string_view name, auto value) -> bool
{
    auto text = detail::history_wire_text(value);
    if (!text) {
        return false;
    }
    object.emplace(name, json(std::string{*text}));
    return true;
}

} // namespace

auto run_list_request_to_json(RunListRequest const& request) -> ConversionResult<JsonValue>
{
    if (auto const* cursor = std::get_if<CursorRequest>(&request)) {
        return ConversionResult<JsonValue>::success(json(JsonValue::Object{
            {"cursor", json(cursor->cursor)}
        }));
    }
    auto const& query = std::get<RunQuery>(request);
    if (!valid_query(query)) {
        return reject<JsonValue>(true);
    }

    auto object = JsonValue::Object{
        {"limit", json(static_cast<std::uint64_t>(query.limit))}
    };
    auto const& filters = query.filters;
    if (filters.queue_id) {
        object.emplace("queue_id", json(filters.queue_id->to_string()));
    }
    if (filters.job_id) {
        object.emplace("job_id", json(filters.job_id->to_string()));
    }
    if ((filters.state && !append_enum(object, "state", *filters.state)) ||
        (filters.origin && !append_enum(object, "origin", *filters.origin)) ||
        (filters.type && !append_enum(object, "type", *filters.type)) ||
        !append_range(object, "planned", filters.planned) || !append_range(object, "started", filters.started) ||
        !append_range(object, "completed", filters.completed)) {
        return reject<JsonValue>(true);
    }
    return ConversionResult<JsonValue>::success(json(std::move(object)));
}

auto run_list_request_from_json(JsonValue const& value) -> ConversionResult<RunListRequest>
{
    if (!value.is_object()) {
        return reject<RunListRequest>(true);
    }
    auto const& object = value.as_object();
    if (auto const* cursor = member(object, "cursor")) {
        if (object.size() != 1 || !cursor->is_string()) {
            return reject<RunListRequest>(true);
        }
        return ConversionResult<RunListRequest>::success(CursorRequest{cursor->as_string()});
    }
    if (!only_members(object,
                      {"queue_id", "job_id", "state", "origin", "type", "planned", "started", "completed", "limit"})) {
        return reject<RunListRequest>(true);
    }

    auto query = RunQuery{};
    if (auto const* limit = member(object, "limit"); limit && !decode_limit(*limit, query.limit)) {
        return reject<RunListRequest>(true);
    }
    if (auto const* queue_id = member(object, "queue_id")) {
        query.filters.queue_id.emplace();
        if (!decode_uuid(*queue_id, *query.filters.queue_id)) {
            return reject<RunListRequest>(true);
        }
    }
    if (auto const* job_id = member(object, "job_id")) {
        query.filters.job_id.emplace();
        if (!decode_uuid(*job_id, *query.filters.job_id)) {
            return reject<RunListRequest>(true);
        }
    }
    if (auto const* state = member(object, "state")) {
        query.filters.state.emplace();
        if (!decode_enum(*state, *query.filters.state)) {
            return reject<RunListRequest>(true);
        }
    }
    if (auto const* origin = member(object, "origin")) {
        query.filters.origin.emplace();
        if (!decode_enum(*origin, *query.filters.origin)) {
            return reject<RunListRequest>(true);
        }
    }
    if (auto const* type = member(object, "type")) {
        query.filters.type.emplace();
        if (!decode_enum(*type, *query.filters.type)) {
            return reject<RunListRequest>(true);
        }
    }
    if ((member(object, "planned") && !decode_range(*member(object, "planned"), query.filters.planned)) ||
        (member(object, "started") && !decode_range(*member(object, "started"), query.filters.started)) ||
        (member(object, "completed") && !decode_range(*member(object, "completed"), query.filters.completed))) {
        return reject<RunListRequest>(true);
    }
    return ConversionResult<RunListRequest>::success(query);
}

auto attempt_list_request_to_json(AttemptListRequest const& request) -> ConversionResult<JsonValue>
{
    if (auto const* cursor = std::get_if<CursorRequest>(&request)) {
        return ConversionResult<JsonValue>::success(json(JsonValue::Object{
            {"cursor", json(cursor->cursor)}
        }));
    }
    auto const& query = std::get<AttemptQuery>(request);
    if (!valid_limit(query.limit)) {
        return reject<JsonValue>(true);
    }
    return ConversionResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id", json(query.run_id.to_string())               },
        {"limit",  json(static_cast<std::uint64_t>(query.limit))}
    }));
}

auto attempt_list_request_from_json(JsonValue const& value) -> ConversionResult<AttemptListRequest>
{
    if (!value.is_object()) {
        return reject<AttemptListRequest>(true);
    }
    auto const& object = value.as_object();
    if (auto const* cursor = member(object, "cursor")) {
        if (object.size() != 1 || !cursor->is_string()) {
            return reject<AttemptListRequest>(true);
        }
        return ConversionResult<AttemptListRequest>::success(CursorRequest{cursor->as_string()});
    }
    if (!only_members(object, {"run_id", "limit"})) {
        return reject<AttemptListRequest>(true);
    }
    auto const* run_id = member(object, "run_id");
    auto        query  = AttemptQuery{};
    if (!run_id || !decode_uuid(*run_id, query.run_id)) {
        return reject<AttemptListRequest>(true);
    }
    if (auto const* limit = member(object, "limit"); limit && !decode_limit(*limit, query.limit)) {
        return reject<AttemptListRequest>(true);
    }
    return ConversionResult<AttemptListRequest>::success(query);
}

auto run_summary_to_json(RunSummary const& summary) -> ConversionResult<JsonValue>
{
    auto origin    = detail::history_wire_text(summary.origin);
    auto type      = detail::history_wire_text(summary.type);
    auto state     = detail::history_wire_text(summary.state);
    auto planned   = encode_time(summary.planned_at);
    auto runnable  = encode_time(summary.runnable_at);
    auto started   = encode_nullable_time(summary.started_at);
    auto completed = encode_nullable_time(summary.completed_at);
    if (!origin || !type || !state || !planned || !runnable || !started || !completed || summary.job_revision == 0) {
        return reject<JsonValue>(false);
    }
    return ConversionResult<JsonValue>::success(json(JsonValue::Object{
        {"id",             json(summary.id.to_string())                     },
        {"job_id",         json(summary.job_id.to_string())                 },
        {"queue_id",       json(summary.queue_id.to_string())               },
        {"job_revision",   json(summary.job_revision)                       },
        {"origin",         json(std::string{*origin})                       },
        {"type",           json(std::string{*type})                         },
        {"state",          json(std::string{*state})                        },
        {"schedule_owned", json(summary.schedule_owned)                     },
        {"priority",       json(static_cast<std::int64_t>(summary.priority))},
        {"planned_at",     std::move(*planned)                              },
        {"runnable_at",    std::move(*runnable)                             },
        {"started_at",     std::move(*started)                              },
        {"completed_at",   std::move(*completed)                            },
    }));
}

auto run_summary_from_json(JsonValue const& value) -> ConversionResult<RunSummary>
{
    if (!value.is_object()) {
        return reject<RunSummary>(false);
    }
    auto const& object         = value.as_object();
    auto        result         = RunSummary{};
    auto const* id             = member(object, "id");
    auto const* job_id         = member(object, "job_id");
    auto const* queue_id       = member(object, "queue_id");
    auto const* revision       = member(object, "job_revision");
    auto const* origin         = member(object, "origin");
    auto const* type           = member(object, "type");
    auto const* state          = member(object, "state");
    auto const* schedule_owned = member(object, "schedule_owned");
    auto const* priority       = member(object, "priority");
    auto const* planned        = member(object, "planned_at");
    auto const* runnable       = member(object, "runnable_at");
    auto const* started        = member(object, "started_at");
    auto const* completed      = member(object, "completed_at");

    // Decode identity and the immutable snapshot fields before the mutable lifecycle fields.
    if (!id || !decode_uuid(*id, result.id) || !job_id || !decode_uuid(*job_id, result.job_id) || !queue_id ||
        !decode_uuid(*queue_id, result.queue_id) || !revision || !decode_uint(*revision, result.job_revision) ||
        result.job_revision == 0 || !origin || !decode_enum(*origin, result.origin) || !type ||
        !decode_enum(*type, result.type) || !schedule_owned || !schedule_owned->is_bool() || !priority ||
        !decode_i32(*priority, result.priority) || !planned || !decode_time(*planned, result.planned_at) || !runnable ||
        !decode_time(*runnable, result.runnable_at)) {
        return reject<RunSummary>(false);
    }
    result.schedule_owned = schedule_owned->as_bool();

    if (!state || !decode_enum(*state, result.state) || !started ||
        !decode_nullable_time(*started, result.started_at) || !completed ||
        !decode_nullable_time(*completed, result.completed_at)) {
        return reject<RunSummary>(false);
    }
    return ConversionResult<RunSummary>::success(result);
}

auto run_details_to_json(RunDetails const& details, AttributeRegistry const& registry) -> ConversionResult<JsonValue>
{
    auto summary    = run_summary_to_json(details);
    auto attributes = attribute_set_to_json(details.attributes, registry, AttributeScope::Job);
    if (!summary || !attributes || !details.payload.is_object() || (details.result && !details.result->is_object())) {
        return reject<JsonValue>(false);
    }

    auto  result = details.result ? *details.result : json(jb::core::JsonNull{});
    auto  view   = std::move(summary).value();
    auto& fields = std::get<JsonValue::Object>(view.data);
    fields.emplace("attributes", std::move(attributes).value());
    fields.emplace("payload", details.payload);
    fields.emplace("result", std::move(result));
    if (!jb::core::serialize_json(view)) {
        return reject<JsonValue>(false);
    }
    return ConversionResult<JsonValue>::success(std::move(view));
}

auto run_details_to_json(JobRun const& run, AttributeRegistry const& registry) -> ConversionResult<JsonValue>
{
    auto details                      = RunDetails{};
    static_cast<RunSummary&>(details) = {
        .id             = run.id,
        .job_id         = run.job_id,
        .queue_id       = run.queue_id,
        .job_revision   = run.job_revision,
        .origin         = run.origin,
        .type           = run.type,
        .state          = run.state,
        .schedule_owned = run.schedule_owned,
        .priority       = run.priority,
        .planned_at     = run.planned_at,
        .runnable_at    = run.runnable_at,
        .started_at     = run.started_at,
        .completed_at   = run.completed_at,
    };
    details.attributes = run.attributes;
    details.payload    = run.payload;
    details.result     = run.result;
    return run_details_to_json(details, registry);
}

auto run_details_from_json(JsonValue const& value, AttributeRegistry const& registry) -> ConversionResult<RunDetails>
{
    auto summary = run_summary_from_json(value);
    if (!summary) {
        return reject<RunDetails>(false);
    }

    auto const& object     = value.as_object();
    auto const* attributes = member(object, "attributes");
    auto const* payload    = member(object, "payload");
    auto const* result     = member(object, "result");
    if (!attributes || !payload || !payload->is_object() || !result || (!result->is_null() && !result->is_object())) {
        return reject<RunDetails>(false);
    }
    auto decoded_attributes = attribute_set_from_json(*attributes, registry, AttributeScope::Job);
    if (!decoded_attributes) {
        return reject<RunDetails>(false);
    }
    for (auto const& definition : registry.definitions()) {
        if (definition.scopes.test(AttributeScope::Job) && !decoded_attributes->contains(definition.name)) {
            return reject<RunDetails>(false);
        }
    }

    auto details                      = RunDetails{};
    static_cast<RunSummary&>(details) = std::move(summary).value();
    details.attributes                = std::move(decoded_attributes).value();
    details.payload                   = *payload;
    if (!result->is_null()) {
        details.result = *result;
    }
    return ConversionResult<RunDetails>::success(std::move(details));
}

auto attempt_summary_to_json(AttemptSummary const& summary) -> ConversionResult<JsonValue>
{
    auto state     = detail::history_wire_text(summary.state);
    auto due       = encode_time(summary.due_at);
    auto started   = encode_nullable_time(summary.started_at);
    auto completed = encode_nullable_time(summary.completed_at);
    auto outcome   = summary.outcome ? detail::history_wire_text(*summary.outcome) : std::optional<std::string_view>{};
    if (!state || !due || !started || !completed || !is_valid_attempt_number(summary.attempt_number) ||
        (summary.outcome && !outcome)) {
        return reject<JsonValue>(false);
    }
    return ConversionResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id",         json(summary.run_id.to_string())                                  },
        {"attempt_number", json(summary.attempt_number)                                      },
        {"due_at",         std::move(*due)                                                   },
        {"started_at",     std::move(*started)                                               },
        {"completed_at",   std::move(*completed)                                             },
        {"state",          json(std::string{*state})                                         },
        {"outcome",        outcome ? json(std::string{*outcome}) : json(jb::core::JsonNull{})},
    }));
}

auto attempt_summary_from_json(JsonValue const& value) -> ConversionResult<AttemptSummary>
{
    if (!value.is_object()) {
        return reject<AttemptSummary>(false);
    }
    auto const& object    = value.as_object();
    auto        result    = AttemptSummary{};
    auto const* run_id    = member(object, "run_id");
    auto const* number    = member(object, "attempt_number");
    auto const* due       = member(object, "due_at");
    auto const* started   = member(object, "started_at");
    auto const* completed = member(object, "completed_at");
    auto const* state     = member(object, "state");
    auto const* outcome   = member(object, "outcome");

    if (!run_id || !decode_uuid(*run_id, result.run_id) || !number || !decode_uint(*number, result.attempt_number) ||
        !is_valid_attempt_number(result.attempt_number) || !due || !decode_time(*due, result.due_at)) {
        return reject<AttemptSummary>(false);
    }
    if (!started || !decode_nullable_time(*started, result.started_at) || !completed ||
        !decode_nullable_time(*completed, result.completed_at) || !state || !decode_enum(*state, result.state) ||
        !outcome) {
        return reject<AttemptSummary>(false);
    }
    if (!outcome->is_null()) {
        result.outcome.emplace();
        if (!decode_enum(*outcome, *result.outcome)) {
            return reject<AttemptSummary>(false);
        }
    }
    return ConversionResult<AttemptSummary>::success(result);
}

} // namespace jb::jobu
