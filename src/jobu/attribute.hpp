/** @file attribute.hpp
 * @brief Defines JobU attribute names, values, definitions, and registry contracts.
 */
#pragma once

#include "byte_buffer.hpp"
#include "enum_bitmask.hpp"
#include "error.hpp"
#include "event_loop_types.hpp"
#include "result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::jobu {

/// Canonical lower-case dotted identifier for an attribute.
using AttributeName = std::string;

/// Scopes in which an attribute definition may accept a value.
enum class AttributeScope : std::uint8_t {
    DaemonDefault = 0x01,
    QueueDefault  = 0x02,
    Job           = 0x04,
};
/// Set of AttributeScope values accepted by an attribute definition.
using AttributeScopes = jb::core::enum_bitmask<AttributeScope>;

/// Project-owned recursive value for a JobU attribute.
/// Absence from an AttributeSet means a partial input did not supply the value; there is no generic null.
struct AttributeValue {
    /// Ordered collection of attribute values.
    using List = std::vector<AttributeValue>;
    /// String-keyed collection of attribute values.
    using Map  = std::map<std::string, AttributeValue, std::less<>>;
    /// Storage alternatives supported by an attribute value.
    using Data =
        std::variant<bool, std::int64_t, double, std::string, jb::core::Duration, jb::core::ByteBuffer, List, Map>;

    /// Stored attribute value.
    Data data;
};

/// Name-keyed collection of partial or fully materialized attribute values.
using AttributeSet = std::map<AttributeName, AttributeValue, std::less<>>;

/// Declared storage type for an AttributeDefinition.
enum class AttributeType : std::uint8_t {
    Boolean,
    Integer,
    Number,
    String,
    Duration,
    Bytes,
    List,
    Map,
};

/// Metadata and built-in policy for one known JobU attribute.
/// Registries validate names, scopes, and values without exposing serialization details.
struct AttributeDefinition {
    /// Canonical name accepted by the registry.
    AttributeName   name;
    /// Required storage alternative for supplied values.
    AttributeType   type;
    /// Scopes in which callers may supply a value.
    AttributeScopes scopes;
    /// Value used when materializing an omitted attribute.
    AttributeValue  built_in_default;
    /// Human-readable explanation of the attribute.
    std::string     description;
    /// Whether future adapters must handle the value as sensitive.
    bool            sensitive{false};
};

/// Validates canonical lower-case dotted attribute-name syntax.
/// @return True when each segment starts with `[a-z]` then uses `[a-z0-9_]*`.
[[nodiscard]] auto is_valid_attribute_name(std::string_view name) noexcept -> bool;

/// Interface for known attribute definitions and domain validation.
/// Services use a registry to reject unknown names without coupling to JSON or backend types.
class AttributeRegistry {
public:
    /// Destroys a registry through its interface.
    virtual ~AttributeRegistry() = default;

    /// Finds a definition by canonical name, or returns null when it is unknown.
    [[nodiscard]] virtual auto find(std::string_view name) const noexcept -> AttributeDefinition const* = 0;
    /// Validates a supplied value for a name and scope.
    /// @return Success or an Error with a stable `jobu.attribute.*` code.
    [[nodiscard]] virtual auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> jb::core::Result<void, jb::core::Error>                                         = 0;
    /// Returns all definitions owned by this registry.
    [[nodiscard]] virtual auto definitions() const -> std::span<AttributeDefinition const> = 0;
};

} // namespace jb::jobu
