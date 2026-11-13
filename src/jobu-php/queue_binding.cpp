#include "queue_binding.hpp"

#include "attribute_binding.hpp"
#include "client_binding.hpp"
#include "resource_state.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/text_validation.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
using State = ResourceState<jobu::Queue>;

auto invalid(std::string message) -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = std::move(message)};
}

auto decode(jobu::ControlReply const& reply) -> ::php::Result<jobu::Queue>
{
    if (auto const* queue = std::get_if<jobu::Queue>(&reply)) {
        return *queue;
    }
    return invalid("Expected a queue reply");
}

template <typename Request>
auto operation(std::string name,
               Operation::Accepted (jobu::ControlClient::*method)(Request const&, jobu::ControlCallOptions),
               Request request) -> ::php::Result<Operation>
{
    auto result = Operation::bind(std::move(name), method, std::move(request));
    if (!result) {
        auto const& error = result.error();
        if (error.category == core::ErrorCategory::InvalidArgument) {
            return invalid(error.message);
        }

        // Entropy acquisition can fail before a create is submitted. It is an
        // operational error, not an invalid PHP argument or an uncertain write.
        auto* entry = snapshot::entry("jobu\\exception");
        ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Unable to prepare the JobU operation"}, entry);
        zend_update_property_stringl(entry,
                                     EG(exception),
                                     ZEND_STRL("_errorCode"),
                                     error.code.data(),
                                     error.code.size());
        zend_update_property_string(entry, EG(exception), ZEND_STRL("_category"), category_name(error.category));
        return ::php::pending_exception();
    }
    return std::move(result).value();
}

// Scalar validation belongs at the PHP boundary; daemon admission remains authoritative.
// No daemon-private validator or persistence implementation is linked into this module.
auto name_argument(::php::Arguments const& args, std::uint32_t index) -> ::php::Result<std::string>
{
    auto text = args.string(index);
    if (!text) {
        return text.error();
    }
    auto name     = std::string(text.value().bytes());
    auto marker   = name.rfind("-deleted#");
    bool reserved = marker != std::string::npos && core::Uuid::parse(std::string_view(name).substr(marker + 9));
    if (name.empty() || name.size() > 128 || !core::is_valid_utf8(name) || core::has_ascii_control(name) || reserved) {
        return invalid("Invalid queue name");
    }
    return name;
}

auto positive_argument(::php::Arguments const& args, std::uint32_t index) -> ::php::Result<std::uint32_t>
{
    auto value = args.integer(index);
    if (!value) {
        return value.error();
    }
    if (value.value() < 1 || std::cmp_greater(value.value(), std::numeric_limits<std::uint32_t>::max())) {
        return invalid("Queue weight and capacity must be positive 32-bit unsigned integers");
    }
    return static_cast<std::uint32_t>(value.value());
}

auto policy_argument(::php::ValueView input) -> ::php::Result<jobu::RecoveryPolicy>
{
    auto value = snapshot::enum_text(input, "jobu\\recoverypolicy");
    if (!value) {
        return value.error();
    }
    return value.value() == "fail_interrupted" ? jobu::RecoveryPolicy::FailInterrupted
                                               : jobu::RecoveryPolicy::RetryInterrupted;
}

auto warning_argument(::php::ValueView input) -> ::php::Result<std::chrono::milliseconds>
{
    auto value = duration_milliseconds(input);
    if (!value) {
        return value.error();
    }
    if (value.value().count() < 0) {
        return invalid("Queue durations must be nonnegative");
    }
    return value.value();
}

auto retention_argument(::php::ValueView input) -> ::php::Result<std::optional<std::chrono::seconds>>
{
    if (!snapshot::present(input)) {
        return std::optional<std::chrono::seconds>{};
    }
    auto value = warning_argument(input);
    if (!value) {
        return value.error();
    }
    if (value.value().count() % 1000 != 0) {
        return invalid("History retention must contain whole seconds");
    }
    return std::optional{std::chrono::duration_cast<std::chrono::seconds>(value.value())};
}

class Queue final {
public:
    Queue(::php::Value client, jobu::Queue snapshot, ::php::Value defaults) noexcept
        : _state(std::move(client), std::move(snapshot))
        , _defaults(std::move(defaults))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void
    {
        _state.trace(visitor);
        visitor.add(_defaults.view());
    }

    auto state() noexcept -> State& { return _state; }

    auto defaults() const noexcept -> ::php::ValueView { return _defaults.view(); }

    auto attach(::php::ValueView owner) -> ::php::Result<void>
    {
        // The attached root retains owner, so callbacks may borrow this native object.
        // Notifications only synchronize net edits; conversion/validation belongs to save.
        return attach_attributes(
            Z_OBJ_P(_defaults.get()),
            owner,
            [this] {
                auto values = attribute_snapshot(_defaults.view());
                auto changed =
                    _state.set("defaults", &jobu::Queue::defaults, std::move(values).value(), equal_attributes);
                (void)changed;
            },
            [this] { return _state.editable(); });
    }

    auto prepare(jobu::Queue const& value) -> ::php::Result<ConfigurationRestore>
    {
        // Check every PHP projection before adopting any part of the new snapshot.
        // Native retention seconds have a wider range than PHP's millisecond Duration.
        if (value.history_retention &&
            value.history_retention->count() > std::numeric_limits<std::int64_t>::max() / 1000) {
            return invalid("History retention exceeds the Duration range");
        }
        // Native clock bounds fit PHP's date range; reject precision loss without
        // allocating Zend objects or entering PHP during prepared restoration.
        auto exact = [](core::UtcTimePoint instant) {
            return std::chrono::duration_cast<std::chrono::microseconds>(instant.time_since_epoch()) ==
                   instant.time_since_epoch();
        };
        if (!exact(value.created_at) || !exact(value.updated_at) || (value.deleted_at && !exact(*value.deleted_at))) {
            return invalid("Queue timestamps must have exact microsecond precision");
        }
        return prepare_attributes_restore(Z_OBJ_P(_defaults.get()), value.defaults);
    }

    auto accept(jobu::Queue value) -> ::php::Result<void>
    {
        return _state.accept(std::move(value), [this](auto const& received) { return prepare(received); });
    }

    auto discard() -> ::php::Result<void>
    {
        return _state.discard([this](auto const& received) { return prepare(received); });
    }

    auto save(core::TimePoint deadline) -> ::php::Result<void>
    {
        auto make =
            [this](auto const& current, auto const& baseline, State::Edits const& edits) -> ::php::Result<Operation> {
            jobu::UpdateQueueRequest request{.queue = baseline.id, .expected_revision = baseline.revision};
            if (edits.contains("name")) {
                request.name = current.name;
            }
            if (edits.contains("weight")) {
                request.weight = current.weight;
            }
            if (edits.contains("concurrency_limit")) {
                request.concurrency_limit = current.concurrency_limit;
            }
            if (edits.contains("recovery_policy")) {
                request.recovery_policy = current.recovery_policy;
            }
            if (edits.contains("history_retention")) {
                request.history_retention.emplace(current.history_retention);
            }
            if (edits.contains("runnable_wait_warning")) {
                request.runnable_wait_warning = current.runnable_wait_warning;
            }
            if (edits.contains("defaults")) {
                auto values = attributes_from_php(_defaults.view());
                if (!values) {
                    return values.error();
                }
                request.defaults = std::move(values).value();
            }
            return operation("queue.update", &jobu::ControlClient::update_queue, std::move(request));
        };
        return _state.save(deadline, make, decode, [this](auto const& received) { return prepare(received); });
    }

private:
    State        _state;
    ::php::Value _defaults;
};

using Binding = ::php::ObjectBinding<Queue>;

auto make_queue(::php::ValueView client, jobu::Queue value) -> ::php::Result<::php::Value>
{
    auto identity = resource_identity(value.id, value.id, value.revision);
    if (!identity) {
        return identity.error();
    }
    auto defaults = defaults_to_php(value.defaults);
    if (!defaults) {
        return defaults.error();
    }

    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\queue"));
    auto owner  = ::php::Value::adopt(slot);
    auto native = Binding::construct(Z_OBJ_P(owner.get()),
                                     ::php::Value::retain(client),
                                     std::move(value),
                                     std::move(defaults).value());
    if (!native) {
        return native.error();
    }
    auto prepared = native.value()->prepare(native.value()->state().current());
    if (!prepared) {
        return prepared.error();
    }
    std::move(prepared).value().commit();
    auto attached = native.value()->attach(owner.view());
    if (!attached) {
        return attached.error();
    }
    return owner;
}

auto get_queue(zend_execute_data* frame, bool by_id) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 3);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }

    jobu::GetQueueRequest request;
    if (by_id) {
        auto text = args.string(1);
        if (!text) {
            return text.error();
        }
        auto id = uuid_from_php(args.get(1));
        if (!id) {
            return id.error();
        }
        request.queue = id.value();
    }
    else {
        auto name = name_argument(args, 1);
        if (!name) {
            return name.error();
        }
        request.queue = std::move(name).value();
    }
    if (args.size() >= 2) {
        auto include = args.boolean(2);
        if (!include) {
            return include.error();
        }
        request.include_deleted = include.value();
    }
    auto deadline = operation_deadline(*client.value(), args, 3, start);
    if (!deadline) {
        return deadline.error();
    }
    auto call = operation("queue.get", &jobu::ControlClient::get_queue, request);
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(::php::ValueView(&frame->This), deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    auto value = decode(reply.value());
    if (!value) {
        return value.error();
    }
    if (by_id) {
        auto valid = resource_identity(std::get<core::Uuid>(request.queue), value.value().id, value.value().revision);
        if (!valid) {
            return valid.error();
        }
    }
    return make_queue(::php::ValueView(&frame->This), std::move(value).value());
}

auto create_queue(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 9);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    jobu::CreateQueueRequest request;
    auto                     name = name_argument(args, 1);
    if (!name) {
        return name.error();
    }
    request.name = std::move(name).value();
    if (args.size() >= 2) {
        auto weight = positive_argument(args, 2);
        if (!weight) {
            return weight.error();
        }
        request.weight = weight.value();
    }
    if (args.size() >= 3) {
        auto limit = positive_argument(args, 3);
        if (!limit) {
            return limit.error();
        }
        request.concurrency_limit = limit.value();
    }
    if (args.size() >= 4) {
        auto policy = policy_argument(args.get(4));
        if (!policy) {
            return policy.error();
        }
        request.recovery_policy = policy.value();
    }
    if (snapshot::present(args.get(5))) {
        auto object = args.object(5, snapshot::entry("jobu\\queuedefaults"));
        if (!object) {
            return object.error();
        }
        auto defaults = attributes_from_php(args.get(5));
        if (!defaults) {
            return defaults.error();
        }
        request.defaults = std::move(defaults).value();
    }
    auto retention = retention_argument(args.get(6));
    if (!retention) {
        return retention.error();
    }
    request.history_retention = retention.value();
    if (snapshot::present(args.get(7))) {
        auto warning = warning_argument(args.get(7));
        if (!warning) {
            return warning.error();
        }
        request.runnable_wait_warning = warning.value();
    }
    if (snapshot::present(args.get(8))) {
        auto key = args.string(8);
        if (!key) {
            return key.error();
        }
        request.idempotency_key = std::string(key.value().bytes());
    }
    auto deadline = operation_deadline(*client.value(), args, 9, start);
    if (!deadline) {
        return deadline.error();
    }

    // The descriptor owns copies and generates an omitted key exactly once.
    auto call = operation("queue.create", &jobu::ControlClient::create_queue, std::move(request));
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(::php::ValueView(&frame->This), deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    auto value = decode(reply.value());
    if (!value) {
        return value.error();
    }
    return make_queue(::php::ValueView(&frame->This), std::move(value).value());
}

auto finish(::php::Result<void> const& result) -> void
{
    if (!result) {
        ::php::raise(result.error());
    }
}

template <typename Action>
auto local(zend_execute_data* frame, std::uint32_t count, Action action) noexcept -> void
{
    auto checked = ::php::Arguments(frame).count(count, count);
    if (!checked) {
        ::php::raise(checked.error());
        return;
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    finish(action(*native.value()));
}

template <typename T, typename Parse>
auto setter(zend_execute_data* frame, char const* field, T jobu::Queue::* member, Parse parse) noexcept -> void
{
    local(frame, 1, [&](Queue& queue) -> ::php::Result<void> {
        auto allowed = queue.state().editable();
        if (!allowed) {
            return allowed.error();
        }
        auto value = parse(::php::Arguments(frame));
        if (!value) {
            return value.error();
        }
        // set() rechecks the guard after coercion and changes only its own field.
        return queue.state().set(field, member, std::move(value).value());
    });
}

enum class Rpc : std::uint8_t {
    Save,
    Refresh,
    Suspend,
    Resume,
    Delete
};

auto rpc(zend_execute_data* frame, Rpc kind) -> ::php::Result<void>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, kind == Rpc::Refresh ? 2 : 1);
    if (!count) {
        return count.error();
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        return native.error();
    }
    auto&               queue = *native.value();
    auto&               state = queue.state();
    ::php::Result<void> allowed;
    if (kind == Rpc::Refresh) {
        allowed = resource_guard(state.dirty(), false);
    }
    else if (kind == Rpc::Save) {
        allowed = state.editable();
    }
    else {
        allowed = state.lifecycle_allowed();
    }
    if (!allowed) {
        return allowed.error();
    }
    auto client = native_client(Z_OBJ_P(state.client().get()));
    if (!client) {
        return client.error();
    }
    auto deadline = operation_deadline(*client.value(), args, kind == Rpc::Refresh ? 2 : 1, start);
    if (!deadline) {
        return deadline.error();
    }
    if (kind == Rpc::Save) {
        return queue.save(deadline.value());
    }

    if (kind == Rpc::Refresh) {
        bool historical = false;
        if (args.size() >= 1) {
            auto include = args.boolean(1);
            if (!include) {
                return include.error();
            }
            historical = include.value();
        }
        auto call = operation("queue.get",
                              &jobu::ControlClient::get_queue,
                              jobu::GetQueueRequest{.queue = state.baseline().id, .include_deleted = historical});
        if (!call) {
            return call.error();
        }
        return state.refresh(deadline.value(), call.value(), decode, [&](auto const& value) {
            return queue.prepare(value);
        });
    }

    auto        method = &jobu::ControlClient::delete_queue;
    auto const* name   = "queue.delete";
    if (kind == Rpc::Suspend) {
        method = &jobu::ControlClient::suspend_queue;
        name   = "queue.suspend";
    }
    else if (kind == Rpc::Resume) {
        method = &jobu::ControlClient::resume_queue;
        name   = "queue.resume";
    }
    auto call = operation(name, method, jobu::QueueSelector{state.baseline().id});
    if (!call) {
        return call.error();
    }
    auto reply = resource_call(state.client(), deadline.value(), call.value());
    if (!reply) {
        return reply.error();
    }
    if (kind == Rpc::Delete) {
        if (!std::holds_alternative<jobu::EmptyReply>(reply.value())) {
            return invalid("Expected a deletion reply");
        }
        state.mark_deleted();
        return {};
    }
    auto value = decode(reply.value());
    if (!value) {
        return value.error();
    }
    return queue.accept(std::move(value).value());
}

} // namespace

auto queue_state(zend_object* object) -> ::php::Result<ResourceState<jobu::Queue>*>
{
    auto native = Binding::get(object);
    if (!native) {
        return native.error();
    }
    return &native.value()->state();
}

auto bind_queue_class(zend_class_entry* queue) -> bool
{
    return static_cast<bool>(Binding::bind(queue));
}
} // namespace jb::php

ZEND_METHOD(JobU_Client, getQueue)
{
    [&]() noexcept { jb::php::snapshot::return_value(jb::php::get_queue(execute_data, false), return_value); }();
}

ZEND_METHOD(JobU_Client, getQueueById)
{
    [&]() noexcept { jb::php::snapshot::return_value(jb::php::get_queue(execute_data, true), return_value); }();
}

ZEND_METHOD(JobU_Client, createQueue)
{
    [&]() noexcept { jb::php::snapshot::return_value(jb::php::create_queue(execute_data), return_value); }();
}

ZEND_METHOD(JobU_Queue, __construct)
{
    (void)execute_data;
    (void)return_value;
    []() noexcept { php::raise({.kind = php::ErrorKind::Runtime, .message = "Obtain queues through Client"}); }();
}

ZEND_METHOD(JobU_Queue, getId)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto text = queue.state().current().id.to_string();
        ZVAL_STRINGL(return_value, text.data(), text.size());
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getName)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto const& text = queue.state().current().name;
        ZVAL_STRINGL(return_value, text.data(), text.size());
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getRevision)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_LONG(return_value, static_cast<zend_long>(queue.state().baseline().revision));
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getWeight)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_LONG(return_value, queue.state().current().weight);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getConcurrencyLimit)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_LONG(return_value, queue.state().current().concurrency_limit);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, isDirty)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_BOOL(return_value, queue.state().dirty());
        return {};
    });
}

ZEND_METHOD(JobU_Queue, isDeleted)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_BOOL(return_value, queue.state().deleted());
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getState)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        std::string wire;
        switch (queue.state().current().state) {
            case jb::jobu::QueueState::Active:
                wire = "active";
                break;
            case jb::jobu::QueueState::Suspending:
                wire = "suspending";
                break;
            case jb::jobu::QueueState::Suspended:
                wire = "suspended";
                break;
            case jb::jobu::QueueState::Deleted:
                wire = "deleted";
                break;
        }
        jb::php::snapshot::return_value(jb::php::snapshot::enum_value("jobu\\queuestate", wire), return_value);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getRecoveryPolicy)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto const* wire = queue.state().current().recovery_policy == jb::jobu::RecoveryPolicy::FailInterrupted
                             ? "fail_interrupted"
                             : "retry_interrupted";
        jb::php::snapshot::return_value(jb::php::snapshot::enum_value("jobu\\recoverypolicy", wire), return_value);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getHistoryRetention)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto retention = queue.state().current().history_retention;
        if (!retention) {
            ZVAL_NULL(return_value);
        }
        else {
            jb::php::snapshot::return_value(
                jb::php::duration_to_php(std::chrono::duration_cast<std::chrono::milliseconds>(*retention)),
                return_value);
        }
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getRunnableWaitWarning)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        jb::php::snapshot::return_value(jb::php::duration_to_php(queue.state().current().runnable_wait_warning),
                                        return_value);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getCreatedAt)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        jb::php::snapshot::return_value(jb::php::time_to_php(queue.state().current().created_at), return_value);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getUpdatedAt)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        jb::php::snapshot::return_value(jb::php::time_to_php(queue.state().current().updated_at), return_value);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getDeletedAt)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto time = queue.state().current().deleted_at;
        if (!time) {
            ZVAL_NULL(return_value);
        }
        else {
            jb::php::snapshot::return_value(jb::php::time_to_php(*time), return_value);
        }
        return {};
    });
}

ZEND_METHOD(JobU_Queue, defaults)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        ZVAL_COPY(return_value, queue.defaults().get());
        return {};
    });
}

ZEND_METHOD(JobU_Queue, getDefaults)
{
    jb::php::local(execute_data, 0, [&](jb::php::Queue& queue) -> php::Result<void> {
        // The existing clone policy copies even an intermediate local layer and detaches owner/guards.
        auto* object = Z_OBJ_P(queue.defaults().get());
        auto* copy   = object->handlers->clone_obj(object);
        if (php::has_exception()) {
            if (copy) {
                OBJ_RELEASE(copy);
            }
            return php::pending_exception();
        }
        ZVAL_OBJ(return_value, copy);
        return {};
    });
}

ZEND_METHOD(JobU_Queue, setName)
{
    (void)return_value;
    jb::php::setter(execute_data, "name", &jb::jobu::Queue::name, [](php::Arguments const& args) {
        return jb::php::name_argument(args, 1);
    });
}

ZEND_METHOD(JobU_Queue, setWeight)
{
    (void)return_value;
    jb::php::setter(execute_data, "weight", &jb::jobu::Queue::weight, [](php::Arguments const& args) {
        return jb::php::positive_argument(args, 1);
    });
}

ZEND_METHOD(JobU_Queue, setConcurrencyLimit)
{
    (void)return_value;
    jb::php::setter(execute_data,
                    "concurrency_limit",
                    &jb::jobu::Queue::concurrency_limit,
                    [](php::Arguments const& args) { return jb::php::positive_argument(args, 1); });
}

ZEND_METHOD(JobU_Queue, setRecoveryPolicy)
{
    (void)return_value;
    jb::php::setter(execute_data, "recovery_policy", &jb::jobu::Queue::recovery_policy, [](php::Arguments const& args) {
        return jb::php::policy_argument(args.get(1));
    });
}

ZEND_METHOD(JobU_Queue, setHistoryRetention)
{
    (void)return_value;
    jb::php::setter(execute_data,
                    "history_retention",
                    &jb::jobu::Queue::history_retention,
                    [](php::Arguments const& args) { return jb::php::retention_argument(args.get(1)); });
}

ZEND_METHOD(JobU_Queue, setRunnableWaitWarning)
{
    (void)return_value;
    jb::php::setter(execute_data,
                    "runnable_wait_warning",
                    &jb::jobu::Queue::runnable_wait_warning,
                    [](php::Arguments const& args) { return jb::php::warning_argument(args.get(1)); });
}

ZEND_METHOD(JobU_Queue, setDefaults)
{
    (void)return_value;
    jb::php::local(execute_data, 1, [&](jb::php::Queue& queue) -> php::Result<void> {
        auto allowed = queue.state().editable();
        if (!allowed) {
            return allowed.error();
        }
        return jb::php::replace_defaults(Z_OBJ_P(queue.defaults().get()), php::Arguments(execute_data).get(1));
    });
}

ZEND_METHOD(JobU_Queue, discardChanges)
{
    (void)return_value;
    jb::php::local(execute_data, 0, [](jb::php::Queue& queue) { return queue.discard(); });
}

ZEND_METHOD(JobU_Queue, save)
{
    (void)return_value;
    [&]() noexcept { jb::php::finish(jb::php::rpc(execute_data, jb::php::Rpc::Save)); }();
}

ZEND_METHOD(JobU_Queue, refresh)
{
    (void)return_value;
    [&]() noexcept { jb::php::finish(jb::php::rpc(execute_data, jb::php::Rpc::Refresh)); }();
}

ZEND_METHOD(JobU_Queue, suspend)
{
    (void)return_value;
    [&]() noexcept { jb::php::finish(jb::php::rpc(execute_data, jb::php::Rpc::Suspend)); }();
}

ZEND_METHOD(JobU_Queue, resume)
{
    (void)return_value;
    [&]() noexcept { jb::php::finish(jb::php::rpc(execute_data, jb::php::Rpc::Resume)); }();
}

ZEND_METHOD(JobU_Queue, delete)
{
    (void)return_value;
    [&]() noexcept { jb::php::finish(jb::php::rpc(execute_data, jb::php::Rpc::Delete)); }();
}
