#include "run_binding.hpp"

#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "http_payload_binding.hpp"
#include "result_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/jobu/history_json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <string_view>
#include <utility>

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

    auto summary() const noexcept -> core::JsonValue const& { return _summary; }

private:
    ::php::Value     _client;
    jobu::RunDetails _value;
    core::JsonValue  _summary;
};

using Binding = ::php::ObjectBinding<Run>;

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
    // Validate all projections before exposing a resource, including optional
    // execution metadata. Configuration getters later return detached copies.
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
