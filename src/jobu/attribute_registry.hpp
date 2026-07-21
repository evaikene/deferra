/**
 * @file attribute_registry.hpp
 * @brief Defines JobU's built-in attributes, materialization policy, and public JSON conversion.
 *
 * Use StandardAttributeRegistry for the Phase 3 JobU policy. Attribute layers remain partial until
 * materialize_attributes() combines built-in, daemon, queue, and job values into a complete job set. The JSON helpers
 * convert one partial or complete set using the registry definition and the scope at which that set is used.
 */
#pragma once

#include "attribute.hpp"
#include "json.hpp"

#include <array>

namespace jb::jobu {

/** Registry containing the standard process-wide JobU attribute definitions.
 *
 * Definitions and the pointers returned by find() remain valid for the lifetime of the registry. The registry is
 * immutable after construction and may be read concurrently when callers otherwise satisfy their own synchronization
 * requirements.
 */
class StandardAttributeRegistry final : public AttributeRegistry {
public:
    /// Constructs the fixed Phase 3 standard definitions and built-in defaults.
    StandardAttributeRegistry();

    /** Finds a standard definition by its canonical name.
     * @param name Canonical attribute name to find.
     * @return Stable definition pointer, or null when @p name is not registered.
     */
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override;

    /** Validates one standard attribute at a supplied scope.
     * @param name Canonical attribute name to validate.
     * @param value Attribute value to validate without retaining it.
     * @param scope Layer or job scope at which the value is supplied.
     * @return Success, or an error with a stable `jobu.attribute.*` code.
     */
    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> jb::core::Result<void, jb::core::Error> override;

    /** Returns every standard definition in canonical name order.
     * @return View valid for the lifetime of the registry.
     */
    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override;

    /** Validates a complete materialized standard job attribute set.
     *
     * The set must contain every standard definition exactly once, contain no unknown name, satisfy Job-scope
     * validation, and satisfy all cross-field rules.
     *
     * @param values Complete materialized set to validate without retaining it.
     * @return Success, or an error with a stable `jobu.attribute.*` code.
     */
    [[nodiscard]] auto validate_materialized(AttributeSet const& values) const
        -> jb::core::Result<void, jb::core::Error>;

private:
    std::array<AttributeDefinition, 9> _definitions;
};

/** Materializes the effective attributes for one job.
 *
 * Each supplied layer is validated at its corresponding scope. Values are then applied in built-in, daemon, queue,
 * and job order before the complete Job-scope set and standard cross-field rules are validated. Inputs are copied and
 * are not retained.
 *
 * @param registry Registry defining known attributes, scopes, defaults, and value validation.
 * @param daemon_defaults Partial daemon-default layer.
 * @param queue_defaults Partial queue-default layer.
 * @param job_values Partial job-specific layer.
 * @return Complete lexicographically ordered set, or an error with a stable `jobu.attribute.*` code.
 */
[[nodiscard]] auto materialize_attributes(AttributeRegistry const& registry,
                                          AttributeSet const&      daemon_defaults,
                                          AttributeSet const&      queue_defaults,
                                          AttributeSet const&      job_values)
    -> jb::core::Result<AttributeSet, jb::core::Error>;

/** Encodes an attribute set as the public definition-directed JSON object.
 *
 * Durations use exact signed integer milliseconds and bytes use lower-case hexadecimal. List and map values use
 * natural JSON recursively; nested durations and bytes are rejected because Phase 3 has no element schema with which
 * to decode them. Applicable standard cross-field constraints are also validated; for example, `retry.max_delay`
 * must not be below `retry.initial_delay` when both attributes are supplied.
 *
 * @param values Partial or complete attribute set to encode without retaining it.
 * @param registry Registry defining every top-level attribute.
 * @param scope Scope at which each supplied value must be accepted.
 * @return Owning JSON object, or an error with a stable `jobu.attribute.*` code.
 */
[[nodiscard]] auto attribute_set_to_json(AttributeSet const&      values,
                                         AttributeRegistry const& registry,
                                         AttributeScope scope) -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes the public definition-directed JSON object into an attribute set.
 *
 * The conversion validates every name and value at @p scope, together with applicable standard cross-field
 * constraints; for example, `retry.max_delay` must not be below `retry.initial_delay` when both attributes are
 * supplied. It does not materialize omitted definitions or retain references to @p value. List and map members decode
 * only the natural JSON alternatives because Phase 3 has no nested duration or byte element schema.
 *
 * @param value JSON object to decode without retaining it.
 * @param registry Registry defining every top-level attribute.
 * @param scope Scope at which each decoded value must be accepted.
 * @return Owning lexicographically ordered set, or an error with a stable `jobu.attribute.*` code.
 */
[[nodiscard]] auto attribute_set_from_json(jb::rpc::JsonValue const& value,
                                           AttributeRegistry const&  registry,
                                           AttributeScope scope) -> jb::core::Result<AttributeSet, jb::core::Error>;

} // namespace jb::jobu
