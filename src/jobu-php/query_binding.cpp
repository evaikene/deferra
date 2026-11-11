#include "query_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/jobu/history_json.hpp>
#include <jb/jobu/statistics_json.hpp>
#include <jb/jobu/utc_timestamp.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>

#include <chrono> // IWYU pragma: keep for the fixed 31-day window bound
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace jb::php {
namespace {
using core::JsonValue;
using snapshot::invalid;

// Build a complete candidate before publishing it. Scalar coercion may execute PHP;
// no borrowed input string/array survives a subsequent coercion.
auto add_uuid(JsonValue::Object& fields, ::php::Arguments const& args, std::uint32_t index, std::string const& name)
    -> ::php::Result<void>
{
    if (!snapshot::present(args.get(index))) {
        return {};
    }
    auto text = args.string(index);
    if (!text) {
        return text.error();
    }
    auto id = uuid_from_php(args.get(index));
    if (!id) {
        return id.error();
    }
    if (id.value().is_nil()) {
        return invalid("Query UUID must not be nil");
    }
    fields.emplace(name, JsonValue{.data = id.value().to_string()});
    return {};
}

auto add_enum(JsonValue::Object&      fields,
              ::php::Arguments const& args,
              std::uint32_t           index,
              std::string const&      name,
              std::string_view        type) -> ::php::Result<void>
{
    if (!snapshot::present(args.get(index))) {
        return {};
    }
    auto text = snapshot::enum_text(args.get(index), type);
    if (!text) {
        return text.error();
    }
    fields.emplace(name, JsonValue{.data = std::move(text).value()});
    return {};
}

auto add_range(JsonValue::Object& fields, ::php::Arguments const& args, std::uint32_t index, std::string const& name)
    -> ::php::Result<void>
{
    if (!snapshot::present(args.get(index))) {
        return {};
    }
    auto range = snapshot::read(args.get(index), "jobu\\timerange");
    if (!range) {
        return range.error();
    }
    fields.emplace(name, range.value()->fields);
    return {};
}

auto add_limit(JsonValue::Object& fields, ::php::Arguments const& args, std::uint32_t index) -> ::php::Result<void>
{
    auto limit = args.size() >= index ? args.integer(index) : ::php::Result<zend_long>(100);
    if (!limit) {
        return limit.error();
    }
    if (limit.value() < 1 || limit.value() > 200) {
        return invalid("Query limit must be between 1 and 200");
    }
    fields.emplace("limit", JsonValue{.data = static_cast<std::int64_t>(limit.value())});
    return {};
}

auto add_states(JsonValue::Object& fields, ::php::Arguments const& args) -> ::php::Result<void>
{
    if (!snapshot::present(args.get(4))) {
        return {};
    }
    auto array = args.array(4);
    if (!array) {
        return array.error();
    }
    auto const size = zend_hash_num_elements(array.value().get());
    if (size < 1 || size > 7) {
        return invalid("States must contain between 1 and 7 distinct values");
    }
    if (!zend_array_is_list(const_cast<zend_array*>(array.value().get()))) {
        return invalid("States must be a list");
    }
    auto  values = JsonValue::Array{};
    zval* item   = nullptr;
    ZEND_HASH_FOREACH_VAL(const_cast<zend_array*>(array.value().get()), item)
    {
        auto text = snapshot::enum_text(::php::ValueView(item), "jobu\\runstate");
        if (!text) {
            return text.error();
        }
        values.push_back(JsonValue{.data = std::move(text).value()});
    }
    ZEND_HASH_FOREACH_END();
    fields.emplace("states", JsonValue{.data = std::move(values)});
    return {};
}

auto range_input(::php::Arguments const& args) -> ::php::Result<JsonValue>
{
    auto                              fields = JsonValue::Object{};
    std::optional<core::UtcTimePoint> from;
    for (std::uint32_t index = 1; index <= 2; ++index) {
        if (!snapshot::present(args.get(index))) {
            continue;
        }
        auto instant = time_from_php(args.get(index));
        if (!instant) {
            return instant.error();
        }
        if (index == 1) {
            from = instant.value();
        }
        else if (from && *from >= instant.value()) {
            return invalid("TimeRange requires from to precede to");
        }
        auto text = jobu::format_utc_timestamp(instant.value());
        if (!text) {
            return invalid();
        }
        fields.emplace(index == 1 ? "from" : "to", JsonValue{.data = std::move(text).value()});
    }
    return JsonValue{.data = std::move(fields)};
}

auto query_input(::php::Arguments const& args, bool statistics) -> ::php::Result<JsonValue>
{
    auto fields = JsonValue::Object{};
    auto step   = add_uuid(fields, args, 1, "queue_id");
    if (!step) {
        return step.error();
    }
    step = add_uuid(fields, args, 2, "job_id");
    if (!step) {
        return step.error();
    }

    if (statistics) {
        step = add_enum(fields, args, 3, "type", "jobu\\jobtype");
        if (!step) {
            return step.error();
        }
        step = add_enum(fields, args, 4, "origin", "jobu\\runorigin");
        if (!step) {
            return step.error();
        }
        step = add_range(fields, args, 5, "planned");
        if (!step) {
            return step.error();
        }
        if (args.size() >= 6) {
            auto group = snapshot::enum_text(args.get(6), "jobu\\statisticsgroupby");
            if (!group) {
                return group.error();
            }
            fields.emplace("group_by", JsonValue{.data = std::move(group).value()});
        }
        else {
            fields.emplace("group_by", JsonValue{.data = std::string("none")});
        }
        step = add_limit(fields, args, 7);
    }
    else {
        step = add_enum(fields, args, 3, "state", "jobu\\runstate");
        if (!step) {
            return step.error();
        }
        step = add_states(fields, args);
        if (!step) {
            return step.error();
        }
        step = add_enum(fields, args, 5, "origin", "jobu\\runorigin");
        if (!step) {
            return step.error();
        }
        step = add_enum(fields, args, 6, "type", "jobu\\jobtype");
        if (!step) {
            return step.error();
        }
        step = add_range(fields, args, 7, "planned");
        if (!step) {
            return step.error();
        }
        step = add_range(fields, args, 8, "started");
        if (!step) {
            return step.error();
        }
        step = add_range(fields, args, 9, "completed");
        if (!step) {
            return step.error();
        }
        step = add_limit(fields, args, 10);
    }
    if (!step) {
        return step.error();
    }

    auto wire = JsonValue{.data = std::move(fields)};
    if (statistics) {
        auto decoded = jobu::system_statistics_request_from_json(wire);
        if (!decoded) {
            return invalid("Invalid statistics query");
        }
        auto const& range = std::get<jobu::StatisticsRequest>(decoded.value()).planned;
        // Compare without subtracting distant endpoints, which could overflow the native clock.
        if (range.from && range.to && *range.from <= core::UtcTimePoint::max() - std::chrono::days(31) &&
            *range.to > *range.from + std::chrono::days(31)) {
            return invalid("Statistics window must not exceed 31 days");
        }
    }
    else if (!jobu::run_list_request_from_json(wire)) {
        return invalid("Invalid run query");
    }
    // An explicit unbounded range and omission have the same native meaning.
    for (auto const* name : {"planned", "started", "completed"}) {
        auto found = wire.as_object().find(name);
        if (found != wire.as_object().end() && found->second.as_object().empty()) {
            std::get<JsonValue::Object>(wire.data).erase(name);
        }
    }
    return wire;
}

auto construct_query(zend_execute_data* frame, bool range, bool statistics) noexcept -> void
{
    ::php::Arguments args(frame);
    auto             maximum = statistics ? 7U : 10U;
    if (range) {
        maximum = 2;
    }
    auto count = args.count(0, maximum);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto candidate = range ? range_input(args) : query_input(args, statistics);
    if (!candidate) {
        ::php::raise(candidate.error());
        return;
    }
    auto ready = snapshot::construct(Z_OBJ(frame->This), std::move(candidate).value());
    if (!ready) {
        ::php::raise(ready.error());
    }
}

auto get_states(zend_execute_data* frame, zval* output) noexcept -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto native = snapshot::Binding::get(Z_OBJ(frame->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto const* states = snapshot::field(native.value()->fields, "states");
    if (!states) {
        ZVAL_NULL(output);
        return;
    }
    array_init(output);
    for (auto const& state : states->as_array()) {
        auto item = snapshot::enum_value("jobu\\runstate", state.as_string());
        if (!item) {
            ::php::raise(item.error());
            return;
        }
        auto slot = item.value().release();
        zend_hash_next_index_insert(Z_ARRVAL_P(output), &slot);
    }
}
} // namespace

auto bind_query_classes(zend_class_entry* range, zend_class_entry* runs, zend_class_entry* statistics) -> bool
{
    return snapshot::bind(range) && snapshot::bind(runs) && snapshot::bind(statistics);
}

auto run_query_from_php(::php::ValueView value) -> ::php::Result<jobu::RunQuery>
{
    auto native = snapshot::read(value, "jobu\\runquery");
    if (!native) {
        return native.error();
    }
    auto decoded = jobu::run_list_request_from_json(native.value()->fields);
    if (!decoded) {
        return invalid("Invalid run query");
    }
    return std::get<jobu::RunQuery>(std::move(decoded).value());
}

auto statistics_query_from_php(::php::ValueView value) -> ::php::Result<jobu::StatisticsRequest>
{
    auto native = snapshot::read(value, "jobu\\statisticsquery");
    if (!native) {
        return native.error();
    }
    auto decoded = jobu::system_statistics_request_from_json(native.value()->fields);
    if (!decoded) {
        return invalid("Invalid statistics query");
    }
    return std::get<jobu::StatisticsRequest>(std::move(decoded).value());
}
} // namespace jb::php

ZEND_METHOD(JobU_TimeRange, __construct)
{
    (void)return_value;
    jb::php::construct_query(execute_data, true, false);
}

ZEND_METHOD(JobU_RunQuery, __construct)
{
    (void)return_value;
    jb::php::construct_query(execute_data, false, false);
}

ZEND_METHOD(JobU_StatisticsQuery, __construct)
{
    (void)return_value;
    jb::php::construct_query(execute_data, false, true);
}

ZEND_METHOD(JobU_RunQuery, getStates)
{
    jb::php::get_states(execute_data, return_value);
}

ZEND_METHOD(JobU_TimeRange, getFrom)
{
    jb::php::snapshot::get(execute_data, return_value, "from", jb::php::snapshot::Kind::Date);
}

ZEND_METHOD(JobU_TimeRange, getTo)
{
    jb::php::snapshot::get(execute_data, return_value, "to", jb::php::snapshot::Kind::Date);
}

ZEND_METHOD(JobU_RunQuery, getQueueId)
{
    jb::php::snapshot::get(execute_data, return_value, "queue_id", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_RunQuery, getJobId)
{
    jb::php::snapshot::get(execute_data, return_value, "job_id", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_RunQuery, getState)
{
    jb::php::snapshot::get(execute_data, return_value, "state", jb::php::snapshot::Kind::Enum, "jobu\\runstate");
}

ZEND_METHOD(JobU_RunQuery, getOrigin)
{
    jb::php::snapshot::get(execute_data, return_value, "origin", jb::php::snapshot::Kind::Enum, "jobu\\runorigin");
}

ZEND_METHOD(JobU_RunQuery, getType)
{
    jb::php::snapshot::get(execute_data, return_value, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype");
}

ZEND_METHOD(JobU_RunQuery, getPlanned)
{
    jb::php::snapshot::get(execute_data, return_value, "planned", jb::php::snapshot::Kind::Range);
}

ZEND_METHOD(JobU_RunQuery, getStarted)
{
    jb::php::snapshot::get(execute_data, return_value, "started", jb::php::snapshot::Kind::Range);
}

ZEND_METHOD(JobU_RunQuery, getCompleted)
{
    jb::php::snapshot::get(execute_data, return_value, "completed", jb::php::snapshot::Kind::Range);
}

ZEND_METHOD(JobU_RunQuery, getLimit)
{
    jb::php::snapshot::get(execute_data, return_value, "limit", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsQuery, getQueueId)
{
    jb::php::snapshot::get(execute_data, return_value, "queue_id", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_StatisticsQuery, getJobId)
{
    jb::php::snapshot::get(execute_data, return_value, "job_id", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_StatisticsQuery, getType)
{
    jb::php::snapshot::get(execute_data, return_value, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype");
}

ZEND_METHOD(JobU_StatisticsQuery, getOrigin)
{
    jb::php::snapshot::get(execute_data, return_value, "origin", jb::php::snapshot::Kind::Enum, "jobu\\runorigin");
}

ZEND_METHOD(JobU_StatisticsQuery, getPlanned)
{
    jb::php::snapshot::get(execute_data, return_value, "planned", jb::php::snapshot::Kind::Range);
}

ZEND_METHOD(JobU_StatisticsQuery, getGroupBy)
{
    jb::php::snapshot::get(execute_data,
                           return_value,
                           "group_by",
                           jb::php::snapshot::Kind::Enum,
                           "jobu\\statisticsgroupby");
}

ZEND_METHOD(JobU_StatisticsQuery, getLimit)
{
    jb::php::snapshot::get(execute_data, return_value, "limit", jb::php::snapshot::Kind::Integer);
}
