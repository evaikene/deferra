#include "functions.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/string.hpp>

#include <Zend/zend_exceptions.h>

extern "C" {
#include <ext/date/php_date.h>
}

#include "functions_arginfo.h"

namespace {

auto probe_pending(php::Arguments const& arguments, zval* return_value) noexcept -> void
{
    // A real PHP frame is required: throwing without one is a Zend fatal error.
    php::raise({.kind = php::ErrorKind::Runtime, .message = "original"}, zend_ce_error);
    auto*      original   = EG(exception);
    auto const references = GC_REFCOUNT(original);
    php::raise({.kind = php::ErrorKind::Value, .message = "secondary"});
    php::raise(php::pending_exception());

    auto       date      = php::datetime_immutable({.seconds = 0, .microseconds = 0});
    auto       timestamp = php::as_timestamp(php::ValueView{});
    auto       number    = arguments.integer(2);
    auto       counts    = arguments.count(1, 2);
    bool const preserved = !date && !timestamp && !number && !counts &&
                           date.error().kind == php::ErrorKind::PendingException &&
                           number.error().kind == php::ErrorKind::PendingException && EG(exception) == original &&
                           GC_REFCOUNT(original) == references;
    zend_clear_exception();
    ZVAL_BOOL(return_value, preserved);
}

auto probe_error(php::Arguments const& arguments) noexcept -> void
{
    auto kind = arguments.string(2);
    if (!kind) {
        php::raise(kind.error());
        return;
    }

    php::Error error{.kind = php::ErrorKind::Runtime, .message = "requirement"};
    if (kind.value() == "type") {
        error.kind = php::ErrorKind::Type;
    }
    if (kind.value() == "value") {
        error.kind = php::ErrorKind::Value;
    }
    if (kind.value() == "count") {
        error.kind = php::ErrorKind::ArgumentCount;
    }
    php::raise(error, kind.value() == "custom" ? zend_ce_error : nullptr);
}

// Keep all C++ work behind a non-throwing Zend callback boundary. Operational
// failures raise PHP exceptions; allocation failure remains process-fatal.
auto probe(zend_execute_data* execute_data, zval* return_value) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto const           count = arguments.count(1, 2);
    if (!count) {
        php::raise(count.error());
        return;
    }
    auto operation = arguments.string(1);
    if (!operation) {
        php::raise(operation.error());
        return;
    }

    auto const name = operation.value();
    if (name == "optional") {
        auto const value = arguments.get(2);
        if (!value.get()) {
            ZVAL_STRING(return_value, "missing");
        }
        else if (value.dereference().is_null()) {
            ZVAL_STRING(return_value, "null");
        }
        else {
            ZVAL_COPY(return_value, value.get());
        }
    }
    else if (name == "integer") {
        auto result = arguments.integer(2);
        if (!result) {
            php::raise(result.error());
            return;
        }
        ZVAL_LONG(return_value, result.value());
    }
    else if (name == "float") {
        auto result = arguments.floating(2);
        if (!result) {
            php::raise(result.error());
            return;
        }
        ZVAL_DOUBLE(return_value, result.value());
    }
    else if (name == "boolean") {
        auto result = arguments.boolean(2);
        if (!result) {
            php::raise(result.error());
            return;
        }
        ZVAL_BOOL(return_value, result.value());
    }
    else if (name == "string") {
        auto result = arguments.string(2);
        if (!result) {
            php::raise(result.error());
            return;
        }
        auto owner = php::String::retain(result.value());
        ZVAL_STR(return_value, owner.release());
    }
    else if (name == "array") {
        auto result = arguments.array(2);
        if (!result) {
            php::raise(result.error());
            return;
        }
        ZVAL_LONG(return_value, static_cast<zend_long>(result.value().size()));
    }
    else if (name == "object") {
        auto result = arguments.object(2, php_date_get_interface_ce());
        if (!result) {
            php::raise(result.error());
            return;
        }
        ZVAL_TRUE(return_value);
    }
    else if (name == "date") {
        auto instant = php::as_timestamp(arguments.get(2));
        if (!instant) {
            php::raise(instant.error());
            return;
        }
        auto result = php::datetime_immutable(instant.value());
        if (!result) {
            php::raise(result.error());
            return;
        }
        *return_value = result.value().release();
    }
    else if (name == "pending") {
        probe_pending(arguments, return_value);
    }
    else if (name == "raise") {
        probe_error(arguments);
    }
    else if (name == "range") {
        auto number = arguments.integer(2);
        if (!number) {
            php::raise(number.error());
            return;
        }
        auto result = php::checked_integer<unsigned char>(number.value());
        if (!result) {
            auto error     = result.error();
            error.argument = 2;
            php::raise(error);
            return;
        }
        ZVAL_LONG(return_value, result.value());
    }
    else {
        php::raise({.kind = php::ErrorKind::Value, .message = "unknown test operation", .argument = 1});
    }
}

} // namespace

ZEND_FUNCTION(phpcxx_probe)
{
    probe(execute_data, return_value);
}

auto phpcxx_test_functions() noexcept -> zend_function_entry const*
{
    return ext_functions;
}
