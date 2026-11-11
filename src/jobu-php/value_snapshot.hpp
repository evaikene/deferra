#pragma once

#include <jb/core/json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/value.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace jb::php::snapshot {

/// Request-local immutable native tree. No Zend values or client references are retained.
/// The canonical key is prepared before publication, so comparison never allocates or executes PHP.
struct Value {
    Value(core::JsonValue fields, std::string key) noexcept;
    auto            trace(::php::GcVisitor& visitor) const noexcept -> void;
    core::JsonValue fields;
    std::string     key;
};

struct Policy : ::php::ObjectBindingPolicy<Value> {
    static auto compare(Value const& left, Value const& right) noexcept -> int;
};

using Binding = ::php::ObjectBinding<Value, Policy>;

/// Look up an internal class in the current runtime; never cache across runtime shutdown.
auto entry(std::string_view name) -> zend_class_entry*;
auto invalid(std::string message = "Invalid value snapshot") -> ::php::Error;
auto bind(zend_class_entry* ce) -> bool;
/// Validate JSON and prepare comparison state before constructing the native object.
auto construct(zend_object* object, core::JsonValue fields) -> ::php::Result<void>;
auto make(std::string_view name, core::JsonValue fields) -> ::php::Result<::php::Value>;
auto read(::php::ValueView input, std::string_view name) -> ::php::Result<Value*>;
auto field(core::JsonValue const& fields, std::string_view name) -> core::JsonValue const*;
auto present(::php::ValueView input) -> bool;
auto enum_text(::php::ValueView input, std::string_view name) -> ::php::Result<std::string>;
auto enum_value(std::string_view name, std::string const& wire) -> ::php::Result<::php::Value>;
/// Copy JSON without PHP callbacks. Objects remain objects, including empty objects and numeric keys.
/// Unsigned integers above PHP_INT_MAX become exact decimal strings.
auto to_php(core::JsonValue const& input) -> ::php::Result<::php::Value>;
auto return_value(::php::Result<::php::Value> result, zval* output) -> void;

/// Typed getter projections never coerce malformed optional result fields.
enum class Kind : std::uint8_t {
    Generic,
    String,
    Integer,
    Boolean,
    Date,
    Duration,
    Range,
    Enum
};
auto project(core::JsonValue const* input, Kind kind, std::string_view enum_class = {}) -> ::php::Result<::php::Value>;
auto get(zend_execute_data* frame,
         zval*              output,
         std::string_view   name,
         Kind               kind       = Kind::Generic,
         std::string_view   enum_class = {}) noexcept -> void;

} // namespace jb::php::snapshot
