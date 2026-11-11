#include "value_snapshot.hpp"
#include "client_binding.hpp"
#include "value_binding.hpp"

#include <jb/jobu/utc_timestamp.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/string.hpp>

extern "C" {
#include <Zend/zend_enum.h>
}

#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

namespace jb::php::snapshot {

Value::Value(core::JsonValue fields, std::string key) noexcept
    : fields(std::move(fields))
    , key(std::move(key))
{}

auto Value::trace([[maybe_unused]] ::php::GcVisitor& visitor) const noexcept -> void
{}

auto Policy::compare(Value const& left, Value const& right) noexcept -> int
{
    return left.key.compare(right.key);
}

auto entry(std::string_view name) -> zend_class_entry*
{
    return static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
}

auto invalid(std::string message) -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = std::move(message)};
}

auto bind(zend_class_entry* ce) -> bool
{
    ce->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(ce));
}

auto construct(zend_object* object, core::JsonValue fields) -> ::php::Result<void>
{
    auto encoded = core::serialize_json(fields);
    if (!encoded) {
        return invalid();
    }
    auto native = Binding::construct(object, std::move(fields), std::move(encoded).value());
    if (!native) {
        return native.error();
    }
    return {};
}

auto make(std::string_view name, core::JsonValue fields) -> ::php::Result<::php::Value>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    zval slot{};
    object_init_ex(&slot, entry(name));
    auto owner       = ::php::Value::adopt(slot);
    auto initialized = construct(Z_OBJ_P(owner.get()), std::move(fields));
    if (!initialized) {
        return initialized.error();
    }
    return owner;
}

auto read(::php::ValueView input, std::string_view name) -> ::php::Result<Value*>
{
    auto object = ::php::as_object(input, entry(name));
    if (!object) {
        return object.error();
    }
    return Binding::get(const_cast<zend_object*>(object.value()));
}

auto field(core::JsonValue const& fields, std::string_view name) -> core::JsonValue const*
{
    if (!fields.is_object()) {
        return nullptr;
    }
    auto found = fields.as_object().find(name);
    return found == fields.as_object().end() ? nullptr : &found->second;
}

auto present(::php::ValueView input) -> bool
{
    input = input.dereference();
    return input.is_initialized() && !input.is_null();
}

auto enum_text(::php::ValueView input, std::string_view name) -> ::php::Result<std::string>
{
    auto object = ::php::as_object(input, entry(name));
    if (!object) {
        return object.error();
    }
    auto* value = zend_enum_fetch_case_value(const_cast<zend_object*>(object.value()));
    return std::string(Z_STRVAL_P(value), Z_STRLEN_P(value));
}

auto enum_value(std::string_view name, std::string const& wire) -> ::php::Result<::php::Value>
{
    auto         text   = ::php::String::create(wire);
    zend_object* object = nullptr;
    if (zend_enum_get_case_by_value(&object, entry(name), 0, const_cast<zend_string*>(text.get()), false) == FAILURE) {
        return ::php::has_exception() ? ::php::pending_exception() : invalid();
    }
    zval slot{};
    ZVAL_OBJ_COPY(&slot, object);
    return ::php::Value::adopt(slot);
}

namespace {
// Conversion is bounded independently of the transport parser because native test/adapter
// callers can supply trees directly. Each recursive result has an owner before insertion.
auto copy_json(core::JsonValue const& input, std::size_t depth) -> ::php::Result<::php::Value>
{
    if ((input.is_array() || input.is_object()) && depth >= 64) {
        return invalid("Result nesting exceeds the supported depth");
    }
    zval slot{};
    ZVAL_NULL(&slot);
    if (input.is_null()) {
        return ::php::Value::adopt(slot);
    }
    if (input.is_bool()) {
        ZVAL_BOOL(&slot, input.as_bool());
    }
    else if (input.is_int()) {
        ZVAL_LONG(&slot, input.as_int());
    }
    else if (input.is_uint()) {
        auto number = input.as_uint();
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<zend_long>::max())) {
            ZVAL_LONG(&slot, static_cast<zend_long>(number));
        }
        else {
            auto text = std::to_string(number);
            ZVAL_STRINGL(&slot, text.data(), text.size());
        }
    }
    else if (input.is_double()) {
        ZVAL_DOUBLE(&slot, input.as_double());
    }
    else if (input.is_string()) {
        auto const& text = input.as_string();
        ZVAL_STRINGL(&slot, text.data(), text.size());
    }
    else {
        if (input.is_array()) {
            array_init(&slot);
        }
        else {
            object_init(&slot);
        }
        auto owner = ::php::Value::adopt(slot);
        if (input.is_array()) {
            for (auto const& child : input.as_array()) {
                auto copied = copy_json(child, depth + 1);
                if (!copied) {
                    return copied.error();
                }
                auto value = copied.value().release();
                zend_hash_next_index_insert(Z_ARRVAL_P(owner.get()), &value);
            }
        }
        else {
            for (auto const& [name, child] : input.as_object()) {
                auto copied = copy_json(child, depth + 1);
                if (!copied) {
                    return copied.error();
                }
                auto value = copied.value().release();
                // Direct property-table insertion preserves literal JSON keys without invoking setters.
                zend_hash_str_update(Z_OBJPROP_P(owner.get()), name.data(), name.size(), &value);
            }
        }
        return owner;
    }
    return ::php::Value::adopt(slot);
}
} // namespace

auto to_php(core::JsonValue const& input) -> ::php::Result<::php::Value>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    return copy_json(input, 0);
}

auto return_value(::php::Result<::php::Value> result, zval* output) -> void
{
    if (!result) {
        ::php::raise(result.error());
        return;
    }
    *output = result.value().release();
}

auto project(core::JsonValue const* input, Kind kind, std::string_view enum_class) -> ::php::Result<::php::Value>
{
    if (kind == Kind::Range) {
        return make("jobu\\timerange", input ? *input : core::JsonValue{.data = core::JsonValue::Object{}});
    }
    if (!input || input->is_null()) {
        return ::php::Value::null();
    }
    switch (kind) {
        case Kind::String:
        case Kind::Date:
        case Kind::Enum:
            if (!input->is_string()) {
                return ::php::Value::null();
            }
            break;
        case Kind::Integer:
        case Kind::Duration:
            if ((!input->is_int() && !input->is_uint()) ||
                (input->is_uint() && input->as_uint() > static_cast<std::uint64_t>(INT64_MAX))) {
                return ::php::Value::null();
            }
            break;
        case Kind::Boolean:
            if (!input->is_bool()) {
                return ::php::Value::null();
            }
            break;
        case Kind::Generic:
        case Kind::Range:
            break;
    }
    if (kind == Kind::Date) {
        auto parsed = jobu::parse_utc_timestamp(input->as_string());
        return parsed ? time_to_php(parsed.value()) : ::php::Result<::php::Value>(invalid());
    }
    if (kind == Kind::Duration) {
        auto count = input->is_int() ? input->as_int() : static_cast<std::int64_t>(input->as_uint());
        return duration_to_php(std::chrono::milliseconds(count));
    }
    if (kind == Kind::Enum) {
        return enum_value(enum_class, input->as_string());
    }
    return to_php(*input);
}

auto get(zend_execute_data* frame, zval* output, std::string_view name, Kind kind, std::string_view enum_class) noexcept
    -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    return_value(project(field(native.value()->fields, name), kind, enum_class), output);
}
} // namespace jb::php::snapshot
