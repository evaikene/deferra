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

namespace jb::rpc {

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
 * integer overflow, and non-finite floating-point results are reported with stable `rpc.json.*` errors. Returned errors
 * never contain the input body.
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
 * floating-point values are rejected with stable `rpc.json.*` errors.
 *
 * @param value JSON tree to serialize.
 * @return Complete JSON text, or a dependency-independent error.
 */
[[nodiscard]] auto serialize_json(JsonValue const& value) -> jb::core::Result<std::string, jb::core::Error>;

} // namespace jb::rpc
