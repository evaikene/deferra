#include "statistics_binding.hpp"

#include "client_binding.hpp"
#include "operation.hpp"
#include "page_binding.hpp"
#include "query_binding.hpp"
#include "queue_binding.hpp"
#include "resource_state.hpp"
#include "result_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/jobu/statistics_json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
using core::JsonValue;
using snapshot::field;
using snapshot::invalid;

// The codec has already constrained the tree to statistics fields. Every integer
// in that tree is a count; rejecting overflow here keeps all typed getters total.
auto counts_fit(JsonValue const& value) -> bool
{
    if (value.is_uint()) {
        return value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<zend_long>::max());
    }
    if (value.is_object()) {
        for (auto const& [name, child] : value.as_object()) {
            (void)name;
            if (!counts_fit(child)) {
                return false;
            }
        }
    }
    if (value.is_array()) {
        for (auto const& child : value.as_array()) {
            if (!counts_fit(child)) {
                return false;
            }
        }
    }
    return true;
}

auto initialized(zend_execute_data* frame, std::uint32_t arguments = 0) -> ::php::Result<snapshot::Value*>
{
    auto count = ::php::Arguments(frame).count(arguments, arguments);
    if (!count) {
        return count.error();
    }
    return snapshot::Binding::get(Z_OBJ(frame->This));
}

auto nested(zend_execute_data* frame, std::string_view member, std::string_view class_name)
    -> ::php::Result<::php::Value>
{
    auto value = initialized(frame);
    if (!value) {
        return value.error();
    }
    auto const& child = *field(value.value()->fields, member);
    return child.is_null() ? ::php::Result<::php::Value>(::php::Value::null()) : snapshot::make(class_name, child);
}

auto group_key(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto value = initialized(frame);
    if (!value) {
        return value.error();
    }
    auto const& fields = value.value()->fields;
    auto const& group  = field(fields, "group_by")->as_string();
    auto const& key    = *field(fields, "key");
    if (group == "type") {
        return snapshot::enum_value("jobu\\jobtype", key.as_string());
    }
    if (group == "origin") {
        return snapshot::enum_value("jobu\\runorigin", key.as_string());
    }
    if (group == "state") {
        return snapshot::enum_value("jobu\\runstate", key.as_string());
    }
    return snapshot::to_php(key);
}

auto enum_count(zend_execute_data* frame, std::string_view member, std::string_view enum_class)
    -> ::php::Result<::php::Value>
{
    auto value = initialized(frame, 1);
    if (!value) {
        return value.error();
    }
    auto key = snapshot::enum_text(::php::Arguments(frame).get(1), enum_class);
    if (!key) {
        return key.error();
    }
    return snapshot::to_php(*field(*field(value.value()->fields, member), key.value()));
}

auto duration_number(zend_execute_data* frame, std::string_view member) -> ::php::Result<::php::Value>
{
    auto value = initialized(frame);
    if (!value) {
        return value.error();
    }
    // The native codec emits optional doubles, including a measured zero. Never
    // route these through the integer-millisecond Duration input abstraction.
    return snapshot::to_php(*field(value.value()->fields, member));
}

auto system_info(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 1);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 1, start);
    if (!deadline) {
        return deadline.error();
    }
    auto operation = Operation::bind("system.info", &jobu::ControlClient::get_system_info);
    auto reply     = resource_call(::php::ValueView(&frame->This), deadline.value(), operation);
    if (!reply) {
        return reply.error();
    }
    auto const* info = std::get_if<jobu::SystemInfo>(&reply.value());
    return info ? system_info_to_php(*info) : ::php::Result<::php::Value>(invalid("Expected system information"));
}

auto statistics(zend_execute_data* frame, bool queue_scoped) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 2);
    if (!count) {
        return count.error();
    }
    auto query = snapshot::present(args.get(1)) ? statistics_query_from_php(args.get(1))
                                                : ::php::Result<jobu::StatisticsRequest>(jobu::StatisticsRequest{});
    if (!query) {
        return query.error();
    }

    auto owner  = ::php::ValueView(&frame->This);
    auto source = PageSource{};
    if (queue_scoped) {
        auto queue = queue_state(Z_OBJ(frame->This));
        if (!queue) {
            return queue.error();
        }
        auto id = queue.value()->current().id;
        if (query.value().queue_id && *query.value().queue_id != id) {
            return invalid("Statistics query queueId must match this Queue");
        }
        // This is a retained-history read, even for dirty/deleted resources.
        // The SDK selector owns queue_id; its statistics filters must omit it.
        owner = queue.value()->client();
        query.value().queue_id.reset();
        source = queue_statistics_page_source({.selector = id, .statistics = std::move(query).value()},
                                              statistics_groups_to_php);
    }
    else {
        source = system_statistics_page_source(std::move(query).value(), statistics_groups_to_php);
    }

    auto client = native_client(Z_OBJ_P(owner.get()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }
    return make_page(owner, std::move(source), deadline.value());
}
} // namespace

auto statistics_groups_to_php([[maybe_unused]] ::php::ValueView client, jobu::StatisticsPage const& page)
    -> ::php::Result<::php::Value>
{
    auto wire = jobu::statistics_page_to_json(page);
    if (!wire || !jobu::statistics_page_from_json(wire.value())) {
        return invalid("Invalid statistics snapshot");
    }
    if (!counts_fit(wire.value())) {
        return invalid("Statistics counts exceed the PHP integer range");
    }

    zval slot;
    array_init(&slot);
    auto result = ::php::Value::adopt(slot);
    for (auto const& group : field(wire.value(), "groups")->as_array()) {
        auto fields = group.as_object();
        fields.emplace("group_by", *field(wire.value(), "group_by"));
        auto value = snapshot::make("jobu\\statisticsgroup", JsonValue{.data = std::move(fields)});
        if (!value) {
            return value.error();
        }
        auto item = std::move(value).value().release();
        zend_hash_next_index_insert(Z_ARR_P(result.get()), &item);
    }
    return result;
}

auto statistics_metadata_to_php(jobu::StatisticsPage const& page) -> ::php::Result<::php::Value>
{
    auto wire = jobu::statistics_page_to_json(page);
    if (!wire) {
        return invalid("Invalid statistics metadata");
    }
    // Preflight PHP's date representation before publishing infallible getters.
    for (auto bound : {page.window.from, page.window.to}) {
        if (!bound) {
            return invalid("Statistics window must be resolved");
        }
        auto date = time_to_php(*bound);
        if (!date) {
            return date.error();
        }
    }
    return snapshot::make("jobu\\statisticsmetadata",
                          JsonValue{
                              .data = JsonValue::Object{{"window", *field(wire.value(), "window")},
                                                        {"group_by", *field(wire.value(), "group_by")},
                                                        {"measurement", *field(wire.value(), "measurement")}}
    });
}
} // namespace jb::php

ZEND_METHOD(JobU_Client, getSystemInfo)
{
    jb::php::snapshot::return_value(jb::php::system_info(execute_data), return_value);
}

ZEND_METHOD(JobU_Client, systemStatistics)
{
    jb::php::snapshot::return_value(jb::php::statistics(execute_data, false), return_value);
}

ZEND_METHOD(JobU_Queue, statistics)
{
    jb::php::snapshot::return_value(jb::php::statistics(execute_data, true), return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getKey)
{
    jb::php::snapshot::return_value(jb::php::group_key(execute_data), return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsRunCounts, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsAttemptCounts, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsCaptureCounts, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsDuration, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsRunnableWaitCoverage, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsMetadata, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsMeasurement, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise(jb::php::snapshot::invalid("This snapshot is created by JobU"));
}

ZEND_METHOD(JobU_StatisticsGroup, getRuns)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "runs", "jobu\\statisticsruncounts"), return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getAttempts)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "attempts", "jobu\\statisticsattemptcounts"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getCapture)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "capture", "jobu\\statisticscapturecounts"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getScheduleLateness)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "schedule_lateness_ms", "jobu\\statisticsduration"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getExecutionWallDuration)
{
    jb::php::snapshot::return_value(
        jb::php::nested(execute_data, "execution_wall_duration_ms", "jobu\\statisticsduration"),
        return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getRunnableWait)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "runnable_wait_ms", "jobu\\statisticsduration"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsGroup, getRunnableWaitCoverage)
{
    jb::php::snapshot::return_value(
        jb::php::nested(execute_data, "runnable_wait_coverage", "jobu\\statisticsrunnablewaitcoverage"),
        return_value);
}

ZEND_METHOD(JobU_StatisticsMetadata, getMeasurement)
{
    jb::php::snapshot::return_value(jb::php::nested(execute_data, "measurement", "jobu\\statisticsmeasurement"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsRunCounts, getStateCount)
{
    jb::php::snapshot::return_value(jb::php::enum_count(execute_data, "states", "jobu\\runstate"), return_value);
}

ZEND_METHOD(JobU_StatisticsRunCounts, getTypeCount)
{
    jb::php::snapshot::return_value(jb::php::enum_count(execute_data, "types", "jobu\\jobtype"), return_value);
}

ZEND_METHOD(JobU_StatisticsRunCounts, getOriginCount)
{
    jb::php::snapshot::return_value(jb::php::enum_count(execute_data, "origins", "jobu\\runorigin"), return_value);
}

ZEND_METHOD(JobU_StatisticsAttemptCounts, getStateCount)
{
    jb::php::snapshot::return_value(jb::php::enum_count(execute_data, "states", "jobu\\attemptstate"), return_value);
}

ZEND_METHOD(JobU_StatisticsAttemptCounts, getOutcomeCount)
{
    jb::php::snapshot::return_value(jb::php::enum_count(execute_data, "outcomes", "jobu\\attemptoutcome"),
                                    return_value);
}

ZEND_METHOD(JobU_StatisticsRunCounts, getTotal)
{
    jb::php::snapshot::get(execute_data, return_value, "total", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsAttemptCounts, getTotal)
{
    jb::php::snapshot::get(execute_data, return_value, "total", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsAttemptCounts, getRetries)
{
    jb::php::snapshot::get(execute_data, return_value, "retries", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsCaptureCounts, getTruncatedAttempts)
{
    jb::php::snapshot::get(execute_data, return_value, "truncated_attempts", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsCaptureCounts, getLostAttempts)
{
    jb::php::snapshot::get(execute_data, return_value, "lost_attempts", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsDuration, getSamples)
{
    jb::php::snapshot::get(execute_data, return_value, "samples", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsRunnableWaitCoverage, getComplete)
{
    jb::php::snapshot::get(execute_data, return_value, "complete", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsRunnableWaitCoverage, getPartial)
{
    jb::php::snapshot::get(execute_data, return_value, "partial", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsRunnableWaitCoverage, getUnmeasured)
{
    jb::php::snapshot::get(execute_data, return_value, "unmeasured", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsRunnableWaitCoverage, getUnfinished)
{
    jb::php::snapshot::get(execute_data, return_value, "unfinished", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_StatisticsMetadata, getWindow)
{
    jb::php::snapshot::get(execute_data, return_value, "window", jb::php::snapshot::Kind::Range);
}

ZEND_METHOD(JobU_StatisticsMeasurement, getTiming)
{
    jb::php::snapshot::get(execute_data, return_value, "timing", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_StatisticsMeasurement, getRunnableWait)
{
    jb::php::snapshot::get(execute_data, return_value, "runnable_wait", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_StatisticsMeasurement, getCapture)
{
    jb::php::snapshot::get(execute_data, return_value, "capture", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_StatisticsMetadata, getGroupBy)
{
    jb::php::snapshot::get(execute_data,
                           return_value,
                           "group_by",
                           jb::php::snapshot::Kind::Enum,
                           "jobu\\statisticsgroupby");
}

ZEND_METHOD(JobU_StatisticsDuration, getAverageMilliseconds)
{
    jb::php::snapshot::return_value(jb::php::duration_number(execute_data, "average"), return_value);
}

ZEND_METHOD(JobU_StatisticsDuration, getMaximumMilliseconds)
{
    jb::php::snapshot::return_value(jb::php::duration_number(execute_data, "maximum"), return_value);
}
