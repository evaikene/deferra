/** @file json.hpp
 * @brief Defines the dependency-independent JSON value tree and codec API.
 */
#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::core {

/** Represents the JSON null value without relying on a codec-library type.
 *
 * Use this alternative to distinguish JSON null from empty strings, arrays, and objects.
 */
struct JsonNull {
    /// Compares two stateless JSON null markers.
    /// @param other Null marker to compare.
    /// @return Always true because JsonNull carries no state.
    auto operator==(JsonNull const& other) const -> bool = default;
};

/** Owns one JSON value independently of the private codec implementation.
 *
 * Strings, arrays, and objects own their contents. Callers select alternatives explicitly; no implicit conversion from
 * primitive or container values is provided. Signed integers, unsigned integers, and floating-point values remain
 * distinct alternatives. Objects order keys lexicographically to make serialization deterministic.
 */
struct JsonValue {
    /// Owning sequence of JSON values in source order.
    using Array  = std::vector<JsonValue>;
    /// Owning JSON object with deterministic lexicographic member ordering.
    using Object = std::map<std::string, JsonValue, std::less<>>;
    /// Complete set of dependency-independent JSON alternatives.
    using Data   = std::variant<JsonNull, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>;

    /// Active JSON alternative; a default-constructed value contains JsonNull.
    Data data{JsonNull{}};

    /// Returns true if the active alternative is JsonNull.
    /// @return True when the active alternative is JsonNull.
    [[nodiscard]] auto is_null() const -> bool { return std::holds_alternative<JsonNull>(data); }

    /// Returns true if the active alternative is bool.
    /// @return True when the active alternative is bool.
    [[nodiscard]] auto is_bool() const -> bool { return std::holds_alternative<bool>(data); }

    /// Returns true if the active alternative is std::int64_t.
    /// @return True when the active alternative is std::int64_t.
    [[nodiscard]] auto is_int() const -> bool { return std::holds_alternative<std::int64_t>(data); }

    /// Returns true if the active alternative is std::uint64_t.
    /// @return True when the active alternative is std::uint64_t.
    [[nodiscard]] auto is_uint() const -> bool { return std::holds_alternative<std::uint64_t>(data); }

    /// Returns true if the active alternative is double.
    /// @return True when the active alternative is double.
    [[nodiscard]] auto is_double() const -> bool { return std::holds_alternative<double>(data); }

    /// Returns true if the active alternative is std::string.
    /// @return True when the active alternative is std::string.
    [[nodiscard]] auto is_string() const -> bool { return std::holds_alternative<std::string>(data); }

    /// Returns true if the active alternative is Array.
    /// @return True when the active alternative is Array.
    [[nodiscard]] auto is_array() const -> bool { return std::holds_alternative<Array>(data); }

    /// Returns true if the active alternative is Object.
    /// @return True when the active alternative is Object.
    [[nodiscard]] auto is_object() const -> bool { return std::holds_alternative<Object>(data); }

    /// Returns the active alternative as a bool
    /// @return The active alternative as a bool.
    /// @throw std::bad_variant_access if the active alternative is not bool.
    [[nodiscard]] auto as_bool() const -> bool { return std::get<bool>(data); }

    /// Returns the active alternative as a std::int64_t
    /// @return The active alternative as a std::int64_t.
    /// @throw std::bad_variant_access if the active alternative is not std::int64_t.
    [[nodiscard]] auto as_int() const -> std::int64_t { return std::get<std::int64_t>(data); }

    /// Returns the active alternative as a std::uint64_t
    /// @return The active alternative as a std::uint64_t.
    /// @throw std::bad_variant_access if the active alternative is not std::uint64_t.
    [[nodiscard]] auto as_uint() const -> std::uint64_t { return std::get<std::uint64_t>(data); }

    /// Returns the active alternative as a double
    /// @return The active alternative as a double.
    /// @throw std::bad_variant_access if the active alternative is not double.
    [[nodiscard]] auto as_double() const -> double { return std::get<double>(data); }

    /// Returns the active alternative as a std::string
    /// @return The active alternative as a std::string.
    /// @throw std::bad_variant_access if the active alternative is not std::string.
    [[nodiscard]] auto as_string() const -> std::string const& { return std::get<std::string>(data); }

    /// Returns the active alternative as an Array
    /// @return The active alternative as an Array.
    /// @throw std::bad_variant_access if the active alternative is not Array.
    [[nodiscard]] auto as_array() const -> Array const& { return std::get<Array>(data); }

    /// Returns the active alternative as an Object
    /// @return The active alternative as an Object.
    /// @throw std::bad_variant_access if the active alternative is not Object.
    [[nodiscard]] auto as_object() const -> Object const& { return std::get<Object>(data); }

    /// Compares the active alternative and all owned contents.
    /// @param other JSON value to compare.
    /// @return True when both trees have equal alternatives and contents.
    auto operator==(JsonValue const& other) const -> bool = default;
};

/** Controls resource limits applied while parsing JSON text.
 *
 * Scalar roots have depth zero. A root array or object has depth one, and every nested array or object increases the
 * depth by one. Consequently, a zero limit permits scalar roots but rejects every container.
 */
struct JsonLimits {
    /// Maximum permitted number of simultaneously nested array and object containers.
    std::size_t max_depth{64};
};

/** Parses one complete JSON value into an owning project value.
 *
 * Negative integer tokens, including `-0`, become std::int64_t; non-negative integer tokens become std::uint64_t;
 * tokens containing a fraction or exponent become double. Invalid syntax, UTF-8, duplicate members, excessive depth,
 * integer overflow, and non-finite floating-point results are reported with stable `core.json.*` errors. Returned
 * errors never contain the input body.
 *
 * @param text Complete JSON text to parse; the returned tree does not borrow from it.
 * @param limits Container-nesting limits for this parse.
 * @return The owning JSON tree, or a dependency-independent error.
 */
[[nodiscard]] auto parse_json(std::string_view text, JsonLimits limits = {})
    -> jb::core::Result<JsonValue, jb::core::Error>;

/** Serializes an owning JSON tree to deterministic JSON text.
 *
 * Object members are emitted in lexicographic key order. Invalid UTF-8 in strings or object keys and non-finite
 * floating-point values are rejected with stable `core.json.*` errors.
 *
 * @param value JSON tree to serialize.
 * @return Complete JSON text, or a dependency-independent error.
 */
[[nodiscard]] auto serialize_json(JsonValue const& value) -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::core
