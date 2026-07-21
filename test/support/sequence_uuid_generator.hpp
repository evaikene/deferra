/** @file sequence_uuid_generator.hpp
 * @brief Defines a deterministic finite UUID generator for tests.
 */
#pragma once

#include "uuid.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace jb::test {

/** Single-threaded UUID generator that returns a configured finite sequence.
 *
 * Use this collaborator in service tests that need stable resource identities and explicit exhaustion behavior.
 */
class SequenceUuidGenerator final : public core::UuidGenerator {
public:
    /// Creates a generator by taking ownership of the values returned in order.
    /// @param values UUID sequence to return.
    explicit SequenceUuidGenerator(std::vector<core::Uuid> values)
        : _values{std::move(values)}
    {}

    /// Returns the next configured UUID.
    /// @return The next UUID, or `test.uuid.sequence_exhausted` after the sequence is consumed.
    [[nodiscard]] auto generate() -> core::Result<core::Uuid, core::Error> override
    {
        if (_next >= _values.size()) {
            return core::Result<core::Uuid, core::Error>::failure({
                .category = core::ErrorCategory::ResourceExhausted,
                .code     = "test.uuid.sequence_exhausted",
                .message  = "The configured test UUID sequence is exhausted",
            });
        }
        return core::Result<core::Uuid, core::Error>::success(_values[_next++]);
    }

private:
    std::vector<core::Uuid> _values;
    std::size_t             _next{0};
};

} // namespace jb::test
