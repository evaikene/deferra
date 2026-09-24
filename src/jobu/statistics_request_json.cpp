#include "statistics_json.hpp"

#include "history_scalar_codec_priv.hpp"
#include "management_json.hpp"
#include "utc_timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
            .code     = "jobu.protocol.invalid_request",
            .message  = "The JobU statistics request is invalid"};
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

auto decode_uuid(JsonValue const& value, jb::core::Uuid& id) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = jb::core::Uuid::parse(value.as_string());
    if (!parsed || parsed->is_nil() || parsed->to_string() != value.as_string()) {
        return false;
    }
    id = std::move(parsed).value();
    return true;
}

auto group_text(StatisticsGroupBy value) -> std::optional<std::string_view>
{
    switch (value) {
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

auto parse_group(std::string_view text, StatisticsGroupBy& value) -> bool
{
    for (auto candidate : {StatisticsGroupBy::None,
                           StatisticsGroupBy::Queue,
                           StatisticsGroupBy::Job,
                           StatisticsGroupBy::Type,
                           StatisticsGroupBy::Origin,
                           StatisticsGroupBy::State}) {
        if (group_text(candidate) == text) {
            value = candidate;
            return true;
        }
    }
    return false;
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

auto decode_planned(JsonValue const& value, UtcRange& planned) -> bool
{
    if (!value.is_object() || !only_members(value.as_object(), {"from", "to"})) {
        return false;
    }
    if (auto const* from = member(value.as_object(), "from")) {
        auto decoded = jb::core::UtcTimePoint{};
        if (!decode_time(*from, decoded)) {
            return false;
        }
        planned.from = decoded;
    }
    if (auto const* to = member(value.as_object(), "to")) {
        auto decoded = jb::core::UtcTimePoint{};
        if (!decode_time(*to, decoded)) {
            return false;
        }
        planned.to = decoded;
    }
    return !planned.from || !planned.to || *planned.from < *planned.to;
}

auto encode_planned(UtcRange const& planned) -> std::optional<JsonValue>
{
    if (planned.from && planned.to && *planned.from >= *planned.to) {
        return std::nullopt;
    }
    auto fields = JsonValue::Object{};
    if (planned.from) {
        auto formatted = format_utc_timestamp(*planned.from);
        if (!formatted) {
            return std::nullopt;
        }
        fields.emplace("from", json(std::move(formatted).value()));
    }
    if (planned.to) {
        auto formatted = format_utc_timestamp(*planned.to);
        if (!formatted) {
            return std::nullopt;
        }
        fields.emplace("to", json(std::move(formatted).value()));
    }
    return json(std::move(fields));
}

auto decode_query(JsonValue::Object const& fields, bool queue_scope) -> CodecResult<StatisticsRequest>
{
    auto query = StatisticsRequest{};
    if (auto const* queue_id = member(fields, "queue_id")) {
        auto decoded = jb::core::Uuid{};
        if (!decode_uuid(*queue_id, decoded)) {
            return reject<StatisticsRequest>();
        }
        if (!queue_scope) {
            query.queue_id = decoded;
        }
    }
    if (auto const* job_id = member(fields, "job_id")) {
        auto decoded = jb::core::Uuid{};
        if (!decode_uuid(*job_id, decoded)) {
            return reject<StatisticsRequest>();
        }
        query.job_id = decoded;
    }
    if (auto const* type = member(fields, "type")) {
        auto decoded = JobType{};
        if (!type->is_string() || !detail::parse_history_wire_text(type->as_string(), decoded)) {
            return reject<StatisticsRequest>();
        }
        query.type = decoded;
    }
    if (auto const* origin = member(fields, "origin")) {
        auto decoded = RunOrigin{};
        if (!origin->is_string() || !detail::parse_history_wire_text(origin->as_string(), decoded)) {
            return reject<StatisticsRequest>();
        }
        query.origin = decoded;
    }
    if (auto const* planned = member(fields, "planned")) {
        if (!decode_planned(*planned, query.planned)) {
            return reject<StatisticsRequest>();
        }
    }
    if (auto const* group_by = member(fields, "group_by")) {
        if (!group_by->is_string() || !parse_group(group_by->as_string(), query.group_by)) {
            return reject<StatisticsRequest>();
        }
    }
    if (auto const* limit = member(fields, "limit")) {
        auto number = std::uint64_t{};
        if (limit->is_uint()) {
            number = limit->as_uint();
        }
        else if (limit->is_int() && limit->as_int() > 0) {
            number = static_cast<std::uint64_t>(limit->as_int());
        }
        if (number < 1U || number > 200U) {
            return reject<StatisticsRequest>();
        }
        query.limit = static_cast<std::size_t>(number);
    }
    return CodecResult<StatisticsRequest>::success(query);
}

auto encode_query(StatisticsRequest const& query, bool queue_scope) -> CodecResult<JsonValue>
{
    auto group   = group_text(query.group_by);
    auto planned = encode_planned(query.planned);
    if (!group || !planned || query.limit < 1U || query.limit > 200U || (queue_scope && query.queue_id.has_value())) {
        return reject<JsonValue>();
    }
    auto fields = JsonValue::Object{
        {"group_by", json(std::string{*group})                    },
        {"limit",    json(static_cast<std::uint64_t>(query.limit))},
        {"planned",  std::move(*planned)                          },
    };
    if (query.queue_id) {
        if (query.queue_id->is_nil()) {
            return reject<JsonValue>();
        }
        fields.emplace("queue_id", json(query.queue_id->to_string()));
    }
    if (query.job_id) {
        if (query.job_id->is_nil()) {
            return reject<JsonValue>();
        }
        fields.emplace("job_id", json(query.job_id->to_string()));
    }
    if (query.type) {
        auto type = detail::history_wire_text(*query.type);
        if (!type) {
            return reject<JsonValue>();
        }
        fields.emplace("type", json(std::string{*type}));
    }
    if (query.origin) {
        auto origin = detail::history_wire_text(*query.origin);
        if (!origin) {
            return reject<JsonValue>();
        }
        fields.emplace("origin", json(std::string{*origin}));
    }
    return CodecResult<JsonValue>::success(json(std::move(fields)));
}

auto cursor_request(JsonValue::Object const& fields) -> CodecResult<CursorRequest>
{
    auto const* cursor = member(fields, "cursor");
    if (cursor == nullptr || !cursor->is_string() || cursor->as_string().empty() || !only_members(fields, {"cursor"})) {
        return reject<CursorRequest>();
    }
    return CodecResult<CursorRequest>::success({.cursor = cursor->as_string()});
}

} // namespace

auto system_statistics_request_to_json(StatisticsListRequest const& request) -> CodecResult<JsonValue>
{
    if (auto const* cursor = std::get_if<CursorRequest>(&request)) {
        if (cursor->cursor.empty()) {
            return reject<JsonValue>();
        }
        return CodecResult<JsonValue>::success(json(JsonValue::Object{
            {"cursor", json(cursor->cursor)}
        }));
    }
    return encode_query(std::get<StatisticsRequest>(request), false);
}

auto system_statistics_request_from_json(JsonValue const& value) -> CodecResult<StatisticsListRequest>
{
    if (!value.is_object()) {
        return reject<StatisticsListRequest>();
    }
    auto const& fields = value.as_object();
    if (member(fields, "cursor")) {
        auto cursor = cursor_request(fields);
        if (!cursor) {
            return reject<StatisticsListRequest>();
        }
        return CodecResult<StatisticsListRequest>::success(std::move(cursor).value());
    }
    if (!only_members(fields, {"queue_id", "job_id", "type", "origin", "planned", "group_by", "limit"})) {
        return reject<StatisticsListRequest>();
    }
    auto query = decode_query(fields, false);
    if (!query) {
        return reject<StatisticsListRequest>();
    }
    return CodecResult<StatisticsListRequest>::success(std::move(query).value());
}

auto queue_statistics_request_to_json(QueueStatisticsListRequest const& request) -> CodecResult<JsonValue>
{
    if (auto const* cursor = std::get_if<CursorRequest>(&request)) {
        if (cursor->cursor.empty()) {
            return reject<JsonValue>();
        }
        return CodecResult<JsonValue>::success(json(JsonValue::Object{
            {"cursor", json(cursor->cursor)}
        }));
    }
    auto const& initial  = std::get<QueueStatisticsQuery>(request);
    auto        selector = queue_selector_to_json(initial.selector);
    auto        query    = encode_query(initial.statistics, true);
    if (!selector || !query) {
        return reject<JsonValue>();
    }
    auto fields = std::move(std::get<JsonValue::Object>(query->data));
    for (auto& [name, field] : std::get<JsonValue::Object>(selector->data)) {
        fields.emplace(name, std::move(field));
    }
    return CodecResult<JsonValue>::success(json(std::move(fields)));
}

auto queue_statistics_request_from_json(JsonValue const& value) -> CodecResult<QueueStatisticsListRequest>
{
    if (!value.is_object()) {
        return reject<QueueStatisticsListRequest>();
    }
    auto const& fields = value.as_object();
    if (member(fields, "cursor")) {
        auto cursor = cursor_request(fields);
        if (!cursor) {
            return reject<QueueStatisticsListRequest>();
        }
        return CodecResult<QueueStatisticsListRequest>::success(std::move(cursor).value());
    }
    if (!only_members(fields, {"queue_id", "queue_name", "job_id", "type", "origin", "planned", "group_by", "limit"}) ||
        (member(fields, "queue_id") == nullptr) == (member(fields, "queue_name") == nullptr)) {
        return reject<QueueStatisticsListRequest>();
    }

    // Reuse the management selector contract without allowing its fields to bypass statistics validation.
    auto selector_fields = JsonValue::Object{};
    if (auto const* id = member(fields, "queue_id")) {
        selector_fields.emplace("queue_id", *id);
    }
    else {
        selector_fields.emplace("queue_name", *member(fields, "queue_name"));
    }
    auto selector = queue_selector_from_json(json(std::move(selector_fields)));
    auto query    = decode_query(fields, true);
    if (!selector || !query) {
        return reject<QueueStatisticsListRequest>();
    }
    return CodecResult<QueueStatisticsListRequest>::success(
        QueueStatisticsQuery{.selector = std::move(selector).value(), .statistics = std::move(query).value()});
}

} // namespace jb::jobu
