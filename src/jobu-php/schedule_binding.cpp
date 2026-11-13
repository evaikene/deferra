#include "client_binding.hpp"
#include "operation.hpp"
#include "resource_state.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/jobu/control_json.hpp>
#include <jb/jobu/schedule_input.hpp>
#include <phpcxx/conversion.hpp>

#include <utility>
#include <variant>

namespace jb::php {
namespace {
auto cron_argument(::php::ValueView input) -> ::php::Result<jobu::CronScheduleInput>
{
    auto object = ::php::as_object(input, snapshot::entry("jobu\\cronschedule"));
    if (!object) {
        return object.error();
    }
    auto schedule = update_schedule_from_php(input);
    if (!schedule) {
        return schedule.error();
    }
    // Copy the existing input representation: omission must reach the daemon
    // unchanged, even when this PHP process uses a different default timezone.
    return std::get<jobu::CronScheduleInput>(std::move(schedule).value());
}

auto validate_schedule(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    auto schedule = cron_argument(args.get(1));
    if (!schedule) {
        return schedule.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    auto operation =
        Operation::bind("schedule.validate", &jobu::ControlClient::validate_schedule, std::move(schedule).value());
    if (!operation) {
        return snapshot::invalid("Invalid schedule request");
    }
    auto reply = resource_call(::php::ValueView(&frame->This), deadline.value(), operation.value());
    if (!reply) {
        return reply.error();
    }
    if (!std::holds_alternative<jobu::ScheduleValidationReply>(reply.value())) {
        return snapshot::invalid("Expected a schedule validation reply");
    }
    zval slot;
    ZVAL_NULL(&slot);
    return ::php::Value::adopt(slot);
}

auto next_occurrences(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             arity = args.count(2, 4);
    if (!arity) {
        return arity.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    auto schedule = cron_argument(args.get(1));
    if (!schedule) {
        return schedule.error();
    }
    auto after = time_from_php(args.get(2));
    if (!after) {
        return after.error();
    }
    auto count = args.size() >= 3 ? args.integer(3) : ::php::Result<zend_long>(5);
    if (!count) {
        return count.error();
    }
    if (count.value() < 1 || count.value() > 200) {
        return snapshot::invalid("Occurrence count must be between 1 and 200");
    }
    auto deadline = operation_deadline(*client.value(), args, 4, start);
    if (!deadline) {
        return deadline.error();
    }

    auto request = jobu::ScheduleNextRequest{.schedule = std::move(schedule).value(),
                                             .after    = after.value(),
                                             .count    = static_cast<std::size_t>(count.value())};
    auto operation =
        Operation::bind("schedule.next", &jobu::ControlClient::next_schedule_occurrences, std::move(request));
    if (!operation) {
        return snapshot::invalid("Invalid schedule preview request");
    }
    auto reply = resource_call(::php::ValueView(&frame->This), deadline.value(), operation.value());
    if (!reply) {
        return reply.error();
    }
    auto* next = std::get_if<jobu::ScheduleNextReply>(&reply.value());
    if (!next || next->occurrences.size() != static_cast<std::size_t>(count.value()) ||
        next->occurrences.front() <= after.value()) {
        return snapshot::invalid("Expected schedule occurrences");
    }

    // Publish only after every UTC instant has converted successfully. No PHP
    // methods are invoked and the caller's mutable DateTime remains untouched.
    zval slot;
    array_init(&slot);
    auto values = ::php::Value::adopt(slot);
    for (auto instant : next->occurrences) {
        auto value = time_to_php(instant);
        if (!value) {
            return value.error();
        }
        auto item = value.value().release();
        zend_hash_next_index_insert(Z_ARR_P(values.get()), &item);
    }
    return values;
}
} // namespace
} // namespace jb::php

ZEND_METHOD(JobU_Client, validateSchedule)
{
    jb::php::snapshot::return_value(jb::php::validate_schedule(execute_data), return_value);
}

ZEND_METHOD(JobU_Client, nextScheduleOccurrences)
{
    jb::php::snapshot::return_value(jb::php::next_occurrences(execute_data), return_value);
}
