#include "resource-bindings.hpp"

#include "attribute_binding.hpp"
#include "client_binding.hpp"
#include "resource_state.hpp"
#include "value_binding.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/value.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "resource_arginfo.h"

namespace jobu_php_test {
namespace {

using State = jb::php::ResourceState<jb::jobu::Queue>;

auto decode(jb::jobu::ControlReply const& reply) -> php::Result<jb::jobu::Queue>
{
    if (auto const* queue = std::get_if<jb::jobu::Queue>(&reply)) {
        return *queue;
    }
    return php::Error{.kind = php::ErrorKind::Value, .message = "Expected a queue reply"};
}

auto get_operation(jb::core::Uuid const& id) -> php::Result<jb::php::Operation>
{
    auto result = jb::php::Operation::bind("queue.get",
                                           &jb::jobu::ControlClient::get_queue,
                                           jb::jobu::GetQueueRequest{.queue = id, .include_deleted = true});
    if (!result) {
        return php::Error{.kind = php::ErrorKind::Runtime, .message = result.error().message};
    }
    return std::move(result).value();
}

class Resource final {
public:
    Resource(php::Value client, jb::jobu::Queue snapshot, php::Value defaults) noexcept
        : _state(std::move(client), std::move(snapshot))
        , _defaults(std::move(defaults))
    {}

    auto trace(php::GcVisitor& visitor) const noexcept -> void
    {
        _state.trace(visitor);
        visitor.add(_defaults.view());
    }

    auto state() noexcept -> State& { return _state; }

    auto defaults() const noexcept -> php::ValueView { return _defaults.view(); }

    auto notifications() const noexcept -> std::uint32_t { return _notifications; }

    auto reject_next_snapshot() noexcept -> void { _reject = true; }

    auto attach(php::ValueView owner) -> php::Result<void>
    {
        return jb::php::attach_attributes(
            Z_OBJ_P(_defaults.get()),
            owner,
            [this] {
                ++_notifications;
                // The initialized attached layer always exports without validation.
                // Whole-layer validation belongs to save, not an intermediate edit.
                auto values  = jb::php::attribute_snapshot(_defaults.view());
                auto changed = _state.set("defaults",
                                          &jb::jobu::Queue::defaults,
                                          std::move(values).value(),
                                          jb::php::equal_attributes);
                (void)changed;
            },
            [this] { return _state.editable(); });
    }

    auto prepare(jb::jobu::Queue const& snapshot) -> php::Result<jb::php::ConfigurationRestore>
    {
        auto replacement = jb::php::prepare_attributes_restore(Z_OBJ_P(_defaults.get()), snapshot.defaults);
        if (!replacement) {
            return replacement.error();
        }
        // Fail after preparing a child to prove preparation has no visible effects.
        if (std::exchange(_reject, false)) {
            return php::Error{.kind = php::ErrorKind::Value, .message = "Injected snapshot conversion failure"};
        }
        return std::move(replacement).value();
    }

    auto discard() -> php::Result<void>
    {
        return _state.discard([this](auto const& snapshot) { return prepare(snapshot); });
    }

    auto refresh(jb::core::TimePoint deadline) -> php::Result<void>
    {
        auto operation = get_operation(_state.baseline().id);
        if (!operation) {
            return operation.error();
        }
        return _state.refresh(deadline, operation.value(), decode, [this](auto const& snapshot) {
            return prepare(snapshot);
        });
    }

    auto save(jb::core::TimePoint deadline) -> php::Result<void>
    {
        auto make = [this](auto const&         current,
                           auto const&         baseline,
                           State::Edits const& edits) -> php::Result<jb::php::Operation> {
            jb::jobu::UpdateQueueRequest request{.queue = baseline.id, .expected_revision = baseline.revision};
            if (edits.contains("name")) {
                request.name = current.name;
            }
            if (edits.contains("defaults")) {
                auto defaults = jb::php::attributes_from_php(_defaults.view());
                if (!defaults) {
                    return defaults.error();
                }
                request.defaults = std::move(defaults).value();
            }
            auto operation =
                jb::php::Operation::bind("queue.update", &jb::jobu::ControlClient::update_queue, std::move(request));
            if (!operation) {
                return php::Error{.kind = php::ErrorKind::Runtime, .message = operation.error().message};
            }
            return std::move(operation).value();
        };
        return _state.save(deadline, make, decode, [this](auto const& snapshot) { return prepare(snapshot); });
    }

private:
    State         _state;
    php::Value    _defaults;
    std::uint32_t _notifications = 0;
    bool          _reject        = false;
};

using Binding = php::ObjectBinding<Resource>;

auto resource(zend_execute_data* frame) noexcept -> Resource*
{
    auto value = Binding::get(Z_OBJ(frame->This));
    if (!value) {
        php::raise(value.error());
        return nullptr;
    }
    return value.value();
}

auto finish(php::Result<void> const& result) -> void
{
    if (!result) {
        php::raise(result.error());
    }
}

auto construct(zend_execute_data* frame) -> php::Result<void>
{
    auto           start = jb::core::Clock::now();
    php::Arguments args(frame);
    auto           client = args.get(1).dereference();
    auto           native = jb::php::native_client(Z_OBJ_P(client.get()));
    if (!native) {
        return native.error();
    }
    auto id = jb::php::uuid_from_php(args.get(2));
    if (!id) {
        return id.error();
    }
    auto deadline = jb::php::operation_deadline(*native.value(), args, 3, start);
    if (!deadline) {
        return deadline.error();
    }
    auto operation = get_operation(id.value());
    if (!operation) {
        return operation.error();
    }
    auto reply = jb::php::resource_call(client, deadline.value(), operation.value());
    if (!reply) {
        return reply.error();
    }
    auto snapshot = decode(reply.value());
    if (!snapshot) {
        return snapshot.error();
    }
    auto identity = jb::php::resource_identity(id.value(), snapshot.value().id, snapshot.value().revision);
    if (!identity) {
        return identity.error();
    }

    // Instantiate the existing native defaults binding before publishing the resource.
    auto* entry =
        static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), ZEND_STRL("jobu\\queuedefaults")));
    zval slot;
    object_init_ex(&slot, entry);
    auto defaults = php::Value::adopt(slot);
    zend_call_known_instance_method_with_0_params(entry->constructor, Z_OBJ_P(defaults.get()), nullptr);
    if (php::has_exception()) {
        return php::pending_exception();
    }
    auto restored = jb::php::prepare_attributes_restore(Z_OBJ_P(defaults.get()), snapshot.value().defaults);
    if (!restored) {
        return restored.error();
    }
    std::move(restored).value().commit();
    auto value = Binding::construct(Z_OBJ(frame->This),
                                    php::Value::retain(client),
                                    std::move(snapshot).value(),
                                    std::move(defaults));
    if (!value) {
        return value.error();
    }
    return value.value()->attach(php::ValueView(&frame->This));
}

auto rpc(zend_execute_data* frame, bool save) -> void
{
    auto  start = jb::core::Clock::now();
    auto* value = resource(frame);
    if (!value) {
        return;
    }
    auto native = jb::php::native_client(Z_OBJ_P(value->state().client().get()));
    if (!native) {
        php::raise(native.error());
        return;
    }
    auto deadline = jb::php::operation_deadline(*native.value(), php::Arguments(frame), 1, start);
    if (!deadline) {
        php::raise(deadline.error());
        return;
    }
    finish(save ? value->save(deadline.value()) : value->refresh(deadline.value()));
}

} // namespace

auto register_resource_classes() noexcept -> zend_result
{
    return Binding::bind(register_class_JobUTestResource()) ? SUCCESS : FAILURE;
}
} // namespace jobu_php_test

ZEND_METHOD(JobUTestResource, __construct)
{
    (void)return_value;
    [&]() noexcept { jobu_php_test::finish(jobu_php_test::construct(execute_data)); }();
}

ZEND_METHOD(JobUTestResource, name)
{
    [&]() noexcept {
        if (auto* value = jobu_php_test::resource(execute_data)) {
            auto const& name = value->state().current().name;
            ZVAL_STRINGL(return_value, name.data(), name.size());
        }
    }();
}

ZEND_METHOD(JobUTestResource, revision)
{
    if (auto* value = jobu_php_test::resource(execute_data)) {
        ZVAL_LONG(return_value, value->state().baseline().revision);
    }
}

ZEND_METHOD(JobUTestResource, dirty)
{
    if (auto* value = jobu_php_test::resource(execute_data)) {
        ZVAL_BOOL(return_value, value->state().dirty());
    }
}

ZEND_METHOD(JobUTestResource, defaults)
{
    if (auto* value = jobu_php_test::resource(execute_data)) {
        ZVAL_COPY(return_value, value->defaults().get());
    }
}

ZEND_METHOD(JobUTestResource, notifications)
{
    if (auto* value = jobu_php_test::resource(execute_data)) {
        ZVAL_LONG(return_value, value->notifications());
    }
}

ZEND_METHOD(JobUTestResource, setName)
{
    (void)return_value;
    [&]() noexcept {
        auto name = php::Arguments(execute_data).string(1);
        if (!name) {
            php::raise(name.error());
            return;
        }
        if (auto* value = jobu_php_test::resource(execute_data)) {
            jobu_php_test::finish(
                value->state().set("name", &jb::jobu::Queue::name, std::string(name.value().bytes())));
        }
    }();
}

ZEND_METHOD(JobUTestResource, save)
{
    (void)return_value;
    [&]() noexcept { jobu_php_test::rpc(execute_data, true); }();
}

ZEND_METHOD(JobUTestResource, refresh)
{
    (void)return_value;
    [&]() noexcept { jobu_php_test::rpc(execute_data, false); }();
}

ZEND_METHOD(JobUTestResource, discardChanges)
{
    (void)return_value;
    [&]() noexcept {
        if (auto* value = jobu_php_test::resource(execute_data)) {
            jobu_php_test::finish(value->discard());
        }
    }();
}

ZEND_METHOD(JobUTestResource, lifecycle)
{
    (void)return_value;
    [&]() noexcept {
        if (auto* value = jobu_php_test::resource(execute_data)) {
            jobu_php_test::finish(value->state().lifecycle_allowed());
        }
    }();
}

ZEND_METHOD(JobUTestResource, markDeleted)
{
    (void)return_value;
    if (auto* value = jobu_php_test::resource(execute_data)) {
        value->state().mark_deleted();
    }
}

ZEND_METHOD(JobUTestResource, rejectNextSnapshot)
{
    (void)return_value;
    if (auto* value = jobu_php_test::resource(execute_data)) {
        value->reject_next_snapshot();
    }
}
