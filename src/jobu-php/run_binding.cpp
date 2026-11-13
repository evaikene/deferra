#include "run_binding.hpp"

#include "attempt_binding.hpp"
#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "history_binding.hpp"
#include "http_payload_binding.hpp"
#include "output_binding.hpp"
#include "resource_state.hpp"
#include "result_binding.hpp"
#include "value_snapshot.hpp"
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/history_json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <string_view>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
class Run final {
public:
    Run(::php::Value client, jobu::RunDetails value, core::JsonValue summary) noexcept
        : _client(std::move(client))
        , _value(std::move(value))
        , _summary(std::move(summary))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto value() const noexcept -> jobu::RunDetails const& { return _value; }

    auto client() const noexcept -> ::php::ValueView { return _client.view(); }

    /// Every projection is prepared before replacing either part of the cache.
    auto accept(jobu::RunDetails value, core::JsonValue summary) noexcept -> void
    {
        _value   = std::move(value);
        _summary = std::move(summary);
    }

    auto summary() const noexcept -> core::JsonValue const& { return _summary; }

private:
    ::php::Value     _client;
    jobu::RunDetails _value;
    core::JsonValue  _summary;
};

using Binding = ::php::ObjectBinding<Run>;

class RunSummary final {
public:
    RunSummary(::php::Value client, jobu::RunSummary value, core::JsonValue fields) noexcept
        : _client(std::move(client))
        , _value(value)
        , _fields(std::move(fields))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto client() const noexcept -> ::php::ValueView { return _client.view(); }

    auto value() const noexcept -> jobu::RunSummary const& { return _value; }

    auto fields() const noexcept -> core::JsonValue const& { return _fields; }

private:
    ::php::Value     _client;
    jobu::RunSummary _value;
    core::JsonValue  _fields;
};

using SummaryBinding = ::php::ObjectBinding<RunSummary>;

auto run_output(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto start  = core::Clock::now();
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto const& run = *native.value();
    return get_output(run.client(), run.value().id, {}, run.value().type, ::php::Arguments(frame), start);
}

auto prepare_run(jobu::RunDetails const& value) -> ::php::Result<core::JsonValue>
{
    // Validate detached configuration and safe results before exposing any new snapshot.
    auto summary = jobu::run_summary_to_json(value);
    if (!summary) {
        return snapshot::invalid("Invalid run reply");
    }
    auto attributes = job_attributes_to_php(value.type, value.attributes);
    if (!attributes) {
        return attributes.error();
    }
    auto payload =
        value.type == jobu::JobType::Cli ? cli_payload_to_php(value.payload) : http_payload_to_php(value.payload);
    if (!payload) {
        return payload.error();
    }
    auto result = execution_result_to_php(value.type, value.result);
    if (!result) {
        return result.error();
    }
    return std::move(summary).value();
}

auto read_summary(zend_execute_data* frame,
                  std::string_view   field,
                  snapshot::Kind     kind,
                  std::string_view   enumeration) noexcept -> ::php::Result<::php::Value>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    auto native = SummaryBinding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    return snapshot::project(snapshot::field(native.value()->fields(), field), kind, enumeration);
}

auto load_summary(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 1);
    if (!count) {
        return count.error();
    }
    auto native = SummaryBinding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& summary  = *native.value();
    auto  deadline = history_deadline(summary.client(), args, 1, start);
    if (!deadline) {
        return deadline.error();
    }
    auto value = read_run_details(summary.client(), summary.value().id, deadline.value());
    if (!value) {
        return value.error();
    }
    if (value.value().type != summary.value().type) {
        return snapshot::invalid("Run reply changed immutable runner type");
    }
    return run_to_php(summary.client(), std::move(value).value());
}

auto get_run_attempt(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        return count.error();
    }
    auto number = args.integer(1);
    if (!number) {
        return number.error();
    }
    if (number.value() < 1) {
        return snapshot::invalid("Attempt number must be positive");
    }

    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto const& run      = *native.value();
    auto        deadline = history_deadline(run.client(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    jobu::AttemptKey key{.run_id = run.value().id, .attempt_number = static_cast<jobu::AttemptNumber>(number.value())};
    return load_attempt(run.client(), key, run.value().type, deadline.value());
}

auto run_operation(zend_execute_data* frame, std::string_view action) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, action == "list" ? 2 : 1);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& run = *native.value();
    if (action == "list") {
        return list_attempt_history(run.client(), run.value().id, args, start, run.value().type);
    }
    auto deadline = history_deadline(run.client(), args, 1, start);
    if (!deadline) {
        return deadline.error();
    }

    auto value       = jobu::RunDetails{};
    auto disposition = ::php::Value::null();
    if (action == "cancel") {
        auto operation = Operation::bind("run.cancel", &jobu::ControlClient::cancel_run, run.value().id);
        if (!operation) {
            return snapshot::invalid("Invalid cancellation request");
        }
        auto reply = resource_call(run.client(), deadline.value(), operation.value());
        if (!reply) {
            return reply.error();
        }
        auto const* cancelled = std::get_if<jobu::CancelRunResult>(&reply.value());
        if (!cancelled) {
            return snapshot::invalid("Expected a cancellation reply");
        }
        // The typed cancellation reply uses JobRun. Convert with the public SDK's
        // full-history codec; no second read or locally fabricated terminal state.
        jobu::StandardAttributeRegistry registry;
        auto                            wire = jobu::run_details_to_json(cancelled->run, registry);
        if (!wire) {
            return snapshot::invalid("Invalid cancelled run");
        }
        auto decoded = jobu::run_details_from_json(wire.value(), registry);
        if (!decoded) {
            return snapshot::invalid("Invalid cancelled run");
        }
        value       = std::move(decoded).value();
        auto result = snapshot::enum_value("jobu\\runcancellationdisposition",
                                           cancelled->disposition == jobu::CancelDisposition::Completed ? "completed"
                                                                                                        : "requested");
        if (!result) {
            return result.error();
        }
        disposition = std::move(result).value();
    }
    else {
        auto fetched = read_run_details(run.client(), run.value().id, deadline.value());
        if (!fetched) {
            return fetched.error();
        }
        value = std::move(fetched).value();
    }
    // Moving a suspended Job updates nonterminal runs' queue/revision. Their
    // stable run/job identity and runner type still cannot change.
    if (value.id != run.value().id || value.type != run.value().type || value.job_id != run.value().job_id) {
        return snapshot::invalid("Run reply changed immutable identity");
    }
    auto summary = prepare_run(value);
    if (!summary) {
        return summary.error();
    }
    run.accept(std::move(value), std::move(summary).value());
    return disposition;
}

auto read_run(zend_execute_data* frame,
              std::string_view   field,
              snapshot::Kind     kind,
              std::string_view   enumeration) noexcept -> ::php::Result<::php::Value>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto const& run = native.value()->value();
    if (field == "attributes") {
        return job_attributes_to_php(run.type, run.attributes);
    }
    if (field == "payload") {
        return run.type == jobu::JobType::Cli ? cli_payload_to_php(run.payload) : http_payload_to_php(run.payload);
    }
    if (field == "result") {
        return execution_result_to_php(run.type, run.result);
    }
    return snapshot::project(snapshot::field(native.value()->summary(), field), kind, enumeration);
}
} // namespace

auto bind_run_class(zend_class_entry* entry) -> bool
{
    entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(entry));
}

auto run_to_php(::php::ValueView client, jobu::RunDetails value) -> ::php::Result<::php::Value>
{
    auto summary = prepare_run(value);
    if (!summary) {
        return summary.error();
    }

    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\run"));
    auto owner   = ::php::Value::adopt(slot);
    auto created = Binding::construct(Z_OBJ_P(owner.get()),
                                      ::php::Value::retain(client),
                                      std::move(value),
                                      std::move(summary).value());
    if (!created) {
        return created.error();
    }
    return owner;
}

auto bind_run_summary_class(zend_class_entry* entry) -> bool
{
    entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(SummaryBinding::bind(entry));
}

auto run_summaries_to_php(::php::ValueView client, jobu::RunPage const& page) -> ::php::Result<::php::Value>
{
    zval slot;
    array_init(&slot);
    auto items = ::php::Value::adopt(slot);
    for (auto const& value : page.items) {
        auto fields = jobu::run_summary_to_json(value);
        if (!fields) {
            return snapshot::invalid("Invalid run summary");
        }
        zval item;
        object_init_ex(&item, snapshot::entry("jobu\\runsummary"));
        auto owner   = ::php::Value::adopt(item);
        auto created = SummaryBinding::construct(Z_OBJ_P(owner.get()),
                                                 ::php::Value::retain(client),
                                                 value,
                                                 std::move(fields).value());
        if (!created) {
            return created.error();
        }
        auto published = owner.release();
        zend_hash_next_index_insert(Z_ARR_P(items.get()), &published);
    }
    return items;
}
} // namespace jb::php

ZEND_METHOD(JobU_Run, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Run resources are created by JobU"});
}

ZEND_METHOD(JobU_Run, getId)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getJobId)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "job_id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getQueueId)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "queue_id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getJobRevision)
{
    jb::php::snapshot::return_value(
        jb::php::read_run(execute_data, "job_revision", jb::php::snapshot::Kind::Integer, ""),
        return_value);
}

ZEND_METHOD(JobU_Run, getPriority)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "priority", jb::php::snapshot::Kind::Integer, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getOrigin)
{
    jb::php::snapshot::return_value(
        jb::php::read_run(execute_data, "origin", jb::php::snapshot::Kind::Enum, "jobu\\runorigin"),
        return_value);
}

ZEND_METHOD(JobU_Run, getType)
{
    jb::php::snapshot::return_value(
        jb::php::read_run(execute_data, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype"),
        return_value);
}

ZEND_METHOD(JobU_Run, getState)
{
    jb::php::snapshot::return_value(
        jb::php::read_run(execute_data, "state", jb::php::snapshot::Kind::Enum, "jobu\\runstate"),
        return_value);
}

ZEND_METHOD(JobU_Run, isScheduleOwned)
{
    jb::php::snapshot::return_value(
        jb::php::read_run(execute_data, "schedule_owned", jb::php::snapshot::Kind::Boolean, ""),
        return_value);
}

ZEND_METHOD(JobU_Run, getPlannedAt)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "planned_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getRunnableAt)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "runnable_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getStartedAt)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "started_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getCompletedAt)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "completed_at", jb::php::snapshot::Kind::Date, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getAttributes)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "attributes", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getPayload)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "payload", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_Run, getResult)
{
    jb::php::snapshot::return_value(jb::php::read_run(execute_data, "result", jb::php::snapshot::Kind::Generic, ""),
                                    return_value);
}

ZEND_METHOD(JobU_RunSummary, getId)
{
    jb::php::snapshot::return_value(jb::php::read_summary(execute_data, "id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_RunSummary, getJobId)
{
    jb::php::snapshot::return_value(jb::php::read_summary(execute_data, "job_id", jb::php::snapshot::Kind::String, ""),
                                    return_value);
}

ZEND_METHOD(JobU_RunSummary, getQueueId)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "queue_id", jb::php::snapshot::Kind::String, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getJobRevision)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "job_revision", jb::php::snapshot::Kind::Integer, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getPriority)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "priority", jb::php::snapshot::Kind::Integer, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getOrigin)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "origin", jb::php::snapshot::Kind::Enum, "jobu\\runorigin"),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getType)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype"),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getState)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "state", jb::php::snapshot::Kind::Enum, "jobu\\runstate"),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, isScheduleOwned)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "schedule_owned", jb::php::snapshot::Kind::Boolean, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getPlannedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "planned_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getRunnableAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "runnable_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getStartedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "started_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, getCompletedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_summary(execute_data, "completed_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_RunSummary, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Run summaries are created by JobU"});
}

ZEND_METHOD(JobU_RunSummary, load)
{
    jb::php::snapshot::return_value(jb::php::load_summary(execute_data), return_value);
}

ZEND_METHOD(JobU_Run, refresh)
{
    jb::php::snapshot::return_value(jb::php::run_operation(execute_data, "refresh"), return_value);
}

ZEND_METHOD(JobU_Run, cancel)
{
    jb::php::snapshot::return_value(jb::php::run_operation(execute_data, "cancel"), return_value);
}

ZEND_METHOD(JobU_Run, listAttempts)
{
    jb::php::snapshot::return_value(jb::php::run_operation(execute_data, "list"), return_value);
}

ZEND_METHOD(JobU_Run, getAttempt)
{
    jb::php::snapshot::return_value(jb::php::get_run_attempt(execute_data), return_value);
}

ZEND_METHOD(JobU_Run, getOutput)
{
    jb::php::snapshot::return_value(jb::php::run_output(execute_data), return_value);
}
