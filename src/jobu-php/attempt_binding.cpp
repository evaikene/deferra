#include "attempt_binding.hpp"

#include "history_binding.hpp"
#include "result_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/jobu/history_json.hpp>
#include <phpcxx/exception.hpp>

#include <utility>

namespace jb::php {
namespace {
// Summary and detail bindings deliberately have distinct native storage. Only a
// complete Attempt owns result data; summaries cannot accidentally expose it.
class AttemptSummary final {
public:
    AttemptSummary(::php::Value client, jobu::AttemptSummary value, core::JsonValue fields, jobu::JobType type) noexcept
        : _client(std::move(client))
        , _value(value)
        , _fields(std::move(fields))
        , _type(type)
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto client() const noexcept -> ::php::ValueView { return _client.view(); }

    auto value() const noexcept -> jobu::AttemptSummary const& { return _value; }

    auto fields() const noexcept -> core::JsonValue const& { return _fields; }

    auto type() const noexcept -> jobu::JobType { return _type; }

private:
    ::php::Value         _client;
    jobu::AttemptSummary _value;
    core::JsonValue      _fields;
    jobu::JobType        _type;
};

class Attempt final {
public:
    Attempt(::php::Value client, jobu::AttemptDetails value, core::JsonValue fields, jobu::JobType type) noexcept
        : _client(std::move(client))
        , _value(std::move(value))
        , _fields(std::move(fields))
        , _type(type)
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto client() const noexcept -> ::php::ValueView { return _client.view(); }

    auto value() const noexcept -> jobu::AttemptDetails const& { return _value; }

    auto fields() const noexcept -> core::JsonValue const& { return _fields; }

    auto type() const noexcept -> jobu::JobType { return _type; }

    /// Commit only after all projections validate, preserving identity and runner metadata.
    auto accept(jobu::AttemptDetails value, core::JsonValue fields) noexcept -> void
    {
        _value  = std::move(value);
        _fields = std::move(fields);
    }

private:
    ::php::Value         _client;
    jobu::AttemptDetails _value;
    core::JsonValue      _fields;
    jobu::JobType        _type;
};

using Binding        = ::php::ObjectBinding<Attempt>;
using SummaryBinding = ::php::ObjectBinding<AttemptSummary>;

auto prepare_attempt(jobu::AttemptDetails const& value, jobu::JobType type) -> ::php::Result<core::JsonValue>
{
    auto fields = jobu::attempt_summary_to_json(value);
    if (!fields) {
        return snapshot::invalid("Invalid attempt reply");
    }
    auto result = execution_result_to_php(type, value.result);
    if (!result) {
        return result.error();
    }
    return std::move(fields).value();
}

template <typename Native>
auto read_attempt(zend_execute_data* frame,
                  std::string_view   field,
                  snapshot::Kind     kind,
                  std::string_view   enumeration) noexcept -> ::php::Result<::php::Value>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    auto native = ::php::ObjectBinding<Native>::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    if constexpr (std::is_same_v<Native, Attempt>) {
        if (field == "result") {
            return execution_result_to_php(native.value()->type(), native.value()->value().result);
        }
    }
    return snapshot::project(snapshot::field(native.value()->fields(), field), kind, enumeration);
}

template <typename Native>
auto attempt_operation(zend_execute_data* frame) noexcept -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 1);
    if (!count) {
        return count.error();
    }
    auto native = ::php::ObjectBinding<Native>::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto& attempt  = *native.value();
    auto  deadline = history_deadline(attempt.client(), args, 1, start);
    if (!deadline) {
        return deadline.error();
    }
    jobu::AttemptKey key{attempt.value().run_id, attempt.value().attempt_number};
    if constexpr (std::is_same_v<Native, AttemptSummary>) {
        return load_attempt(attempt.client(), key, attempt.type(), deadline.value());
    }
    else {
        auto value = read_attempt_details(attempt.client(), key, deadline.value());
        if (!value) {
            return value.error();
        }
        auto fields = prepare_attempt(value.value(), attempt.type());
        if (!fields) {
            return fields.error();
        }
        attempt.accept(std::move(value).value(), std::move(fields).value());
        return ::php::Value::null();
    }
}

auto summaries(::php::ValueView client, jobu::AttemptPage const& page, jobu::JobType type)
    -> ::php::Result<::php::Value>
{
    zval slot;
    array_init(&slot);
    auto items = ::php::Value::adopt(slot);
    for (auto const& value : page.items) {
        auto fields = jobu::attempt_summary_to_json(value);
        if (!fields) {
            return snapshot::invalid("Invalid attempt summary");
        }
        zval item;
        object_init_ex(&item, snapshot::entry("jobu\\attemptsummary"));
        auto owner   = ::php::Value::adopt(item);
        auto created = SummaryBinding::construct(Z_OBJ_P(owner.get()),
                                                 ::php::Value::retain(client),
                                                 value,
                                                 std::move(fields).value(),
                                                 type);
        if (!created) {
            return created.error();
        }
        auto published = owner.release();
        zend_hash_next_index_insert(Z_ARR_P(items.get()), &published);
    }
    return items;
}
} // namespace

auto bind_attempt_classes(zend_class_entry* attempt, zend_class_entry* summary) -> bool
{
    attempt->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    summary->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(attempt)) && static_cast<bool>(SummaryBinding::bind(summary));
}

auto load_attempt(::php::ValueView client, jobu::AttemptKey const& key, jobu::JobType type, core::TimePoint deadline)
    -> ::php::Result<::php::Value>
{
    auto value = read_attempt_details(client, key, deadline);
    if (!value) {
        return value.error();
    }
    // The originating Run supplies trusted immutable metadata; never infer its
    // runner from optional result keys or perform a hidden parent lookup.
    auto fields = prepare_attempt(value.value(), type);
    if (!fields) {
        return fields.error();
    }
    zval slot;
    object_init_ex(&slot, snapshot::entry("jobu\\attempt"));
    auto owner   = ::php::Value::adopt(slot);
    auto created = Binding::construct(Z_OBJ_P(owner.get()),
                                      ::php::Value::retain(client),
                                      std::move(value).value(),
                                      std::move(fields).value(),
                                      type);
    if (!created) {
        return created.error();
    }
    return owner;
}

auto attempt_summary_projection(jobu::JobType type) -> PageProjection<jobu::AttemptPage>
{
    // Function pointers keep the existing PageSource contract free of Zend state.
    // The known enum is copied into each summary, including continuation pages.
    if (type == jobu::JobType::Cli) {
        return +[](::php::ValueView client, jobu::AttemptPage const& page) {
            return summaries(client, page, jobu::JobType::Cli);
        };
    }
    return +[](::php::ValueView client, jobu::AttemptPage const& page) {
        return summaries(client, page, jobu::JobType::Http);
    };
}
} // namespace jb::php

ZEND_METHOD(JobU_Attempt, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Attempts and summaries are created by JobU"});
}

ZEND_METHOD(JobU_Attempt, getRunId)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "run_id", jb::php::snapshot::Kind::String, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, getNumber)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "attempt_number", jb::php::snapshot::Kind::Integer, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, getDueAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "due_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, getStartedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "started_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, getCompletedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "completed_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, getState)
{
    jb::php::snapshot::return_value(jb::php::read_attempt<jb::php::Attempt>(execute_data,
                                                                            "state",
                                                                            jb::php::snapshot::Kind::Enum,
                                                                            "jobu\\attemptstate"),
                                    return_value);
}

ZEND_METHOD(JobU_Attempt, getOutcome)
{
    jb::php::snapshot::return_value(jb::php::read_attempt<jb::php::Attempt>(execute_data,
                                                                            "outcome",
                                                                            jb::php::snapshot::Kind::Enum,
                                                                            "jobu\\attemptoutcome"),
                                    return_value);
}

ZEND_METHOD(JobU_Attempt, getResult)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::Attempt>(execute_data, "result", jb::php::snapshot::Kind::Generic, ""),
        return_value);
}

ZEND_METHOD(JobU_Attempt, refresh)
{
    jb::php::snapshot::return_value(jb::php::attempt_operation<jb::php::Attempt>(execute_data), return_value);
}

ZEND_METHOD(JobU_AttemptSummary, __construct)
{
    (void)execute_data;
    (void)return_value;
    php::raise({.kind = php::ErrorKind::Runtime, .message = "Attempts and summaries are created by JobU"});
}

ZEND_METHOD(JobU_AttemptSummary, getRunId)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::AttemptSummary>(execute_data, "run_id", jb::php::snapshot::Kind::String, ""),
        return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getNumber)
{
    jb::php::snapshot::return_value(jb::php::read_attempt<jb::php::AttemptSummary>(execute_data,
                                                                                   "attempt_number",
                                                                                   jb::php::snapshot::Kind::Integer,
                                                                                   ""),
                                    return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getDueAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::AttemptSummary>(execute_data, "due_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getStartedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::AttemptSummary>(execute_data, "started_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getCompletedAt)
{
    jb::php::snapshot::return_value(
        jb::php::read_attempt<jb::php::AttemptSummary>(execute_data, "completed_at", jb::php::snapshot::Kind::Date, ""),
        return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getState)
{
    jb::php::snapshot::return_value(jb::php::read_attempt<jb::php::AttemptSummary>(execute_data,
                                                                                   "state",
                                                                                   jb::php::snapshot::Kind::Enum,
                                                                                   "jobu\\attemptstate"),
                                    return_value);
}

ZEND_METHOD(JobU_AttemptSummary, getOutcome)
{
    jb::php::snapshot::return_value(jb::php::read_attempt<jb::php::AttemptSummary>(execute_data,
                                                                                   "outcome",
                                                                                   jb::php::snapshot::Kind::Enum,
                                                                                   "jobu\\attemptoutcome"),
                                    return_value);
}

ZEND_METHOD(JobU_AttemptSummary, load)
{
    jb::php::snapshot::return_value(jb::php::attempt_operation<jb::php::AttemptSummary>(execute_data), return_value);
}
