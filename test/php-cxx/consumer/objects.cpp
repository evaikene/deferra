#include "objects.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>

#include <utility>

#include "objects_arginfo.h"

namespace phpcxx_test {
namespace {
// Test accounting is thread-local and owns no Zend state. The binding itself
// has no mutable request globals; counters also remain readable after shutdown.
thread_local Counts native_counts;
} // namespace

auto counts() noexcept -> Counts
{
    return native_counts;
}

Native::Native(zend_long number, php::Value reference) noexcept
    : _number(number)
    , _reference(std::move(reference))
{
    ++native_counts.constructed;
    ++native_counts.live;
}

Native::Native(Native&& other) noexcept
    : Native(other._number, std::move(other._reference))
{}

Native::~Native() noexcept
{
    ++native_counts.destroyed;
    --native_counts.live;
}

auto Native::trace(php::GcVisitor& visitor) const noexcept -> void
{
    visitor.add(_reference.view());
}

auto Native::set_reference(php::Value value) noexcept -> void
{
    _reference = std::move(value);
}

auto ClonePolicy::clone(Native const& source) noexcept -> php::Result<Native>
{
    if (source.number() == -3) {
        php::raise({.kind = php::ErrorKind::Runtime, .message = "original clone exception"});
        return Native(0, php::Value::null());
    }
    if (source.number() < 0) {
        return php::Error{.kind = php::ErrorKind::Value, .message = "Fixture rejects negative native clones"};
    }
    // This fixture deliberately preserves object/reference identity. Real value
    // classes can supply their own deep-copy policy rather than inherit a guess.
    return Native(source.number(), php::Value::retain(source.reference()));
}

auto register_classes() noexcept -> zend_result
{
    auto* node     = register_class_PhpCxxNode();
    auto* locked   = register_class_PhpCxxLocked();
    auto* immobile = register_class_PhpCxxImmobile();
    return Binding::bind(node) && LockedBinding::bind(locked) && ImmobileBinding::bind(immobile) ? SUCCESS : FAILURE;
}

} // namespace phpcxx_test

namespace {

auto initialize_node(zend_execute_data* execute_data) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto                 number = arguments.get(1).get() ? arguments.integer(1) : php::Result<zend_long>(0);
    auto                 fail   = arguments.get(3).get() ? arguments.boolean(3) : php::Result<bool>(false);
    if (!number || !fail) {
        php::raise(!number ? number.error() : fail.error());
        return;
    }
    if (number.value() == -2) {
        php::raise({.kind = php::ErrorKind::Value, .message = "Fixture rejected before native construction"});
        return;
    }
    auto reference = arguments.get(2).get() ? php::Value::retain(arguments.get(2)) : php::Value::null();
    auto result    = phpcxx_test::Binding::construct(Z_OBJ(execute_data->This), number.value(), std::move(reference));
    if (!result) {
        php::raise(result.error());
        return;
    }
    if (fail.value()) {
        // A failure after completed placement construction must still release the
        // native reference when Zend disposes of the failed PHP constructor's object.
        php::raise({.kind = php::ErrorKind::Runtime, .message = "Fixture failed after native construction"});
    }
}

auto native_this(zend_execute_data* execute_data) noexcept -> phpcxx_test::Native*
{
    auto result = phpcxx_test::Binding::get(Z_OBJ(execute_data->This));
    if (!result) {
        php::raise(result.error());
        return nullptr;
    }
    return result.value();
}

auto set_number(zend_execute_data* execute_data) noexcept -> void
{
    auto* native = native_this(execute_data);
    if (!native) {
        return;
    }
    auto number = php::Arguments(execute_data).integer(1);
    if (!number) {
        php::raise(number.error());
        return;
    }
    native->set_number(number.value());
}

} // namespace

ZEND_METHOD(PhpCxxNode, __construct)
{
    (void)return_value;
    initialize_node(execute_data);
}

ZEND_METHOD(PhpCxxNode, number)
{
    if (auto* native = native_this(execute_data)) {
        ZVAL_LONG(return_value, native->number());
    }
}

ZEND_METHOD(PhpCxxNode, setNumber)
{
    (void)return_value;
    set_number(execute_data);
}

ZEND_METHOD(PhpCxxNode, reference)
{
    if (auto* native = native_this(execute_data)) {
        ZVAL_COPY(return_value, native->reference().get());
    }
}

ZEND_METHOD(PhpCxxNode, setReference)
{
    (void)return_value;
    if (auto* native = native_this(execute_data)) {
        native->set_reference(php::Value::retain(php::Arguments(execute_data).get(1)));
    }
}

ZEND_METHOD(PhpCxxNode, accepts)
{
    [&]() noexcept {
        auto value = php::Arguments(execute_data).get(1).dereference();
        if (!value.is_object()) {
            php::raise({.kind = php::ErrorKind::Type, .message = "Expected an object"});
            return;
        }
        auto const accepted = phpcxx_test::Binding::get(Z_OBJ_P(value.get()));
        ZVAL_BOOL(return_value, static_cast<bool>(accepted));
    }();
}

ZEND_METHOD(PhpCxxNode, counts)
{
    (void)execute_data;
    auto const counts = phpcxx_test::counts();
    array_init(return_value);
    add_assoc_long(return_value, "constructed", counts.constructed);
    add_assoc_long(return_value, "destroyed", counts.destroyed);
    add_assoc_long(return_value, "live", counts.live);
}

ZEND_METHOD(PhpCxxLocked, __construct)
{
    (void)return_value;
    // The lambda establishes the same non-throwing boundary as the other handlers.
    [&]() noexcept {
        auto result = phpcxx_test::LockedBinding::construct(Z_OBJ(execute_data->This), 0, php::Value::null());
        if (!result) {
            php::raise(result.error());
        }
    }();
}
