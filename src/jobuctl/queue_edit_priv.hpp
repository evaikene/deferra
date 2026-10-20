#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "json.hpp"
#include "management.hpp"
#include "queue.hpp"
#include "result.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

/// Owns the original queue projection independently of the fetched queue and registry.
/// Const access keeps draft edits separate from the captured identity, revision, and comparison values.
class QueueEditBaseline final {
public:
    /// Projects public encoded values, including only the partial queue-default layer.
    /// Returns a fixed, input-safe error if the queue cannot be encoded.
    [[nodiscard]] static auto from_queue(jb::jobu::Queue const& queue, jb::jobu::AttributeRegistry const& registry)
        -> jb::core::Result<QueueEditBaseline, jb::core::Error>;

    /// Returns the nine-member update-shaped projection, borrowed for this baseline's lifetime.
    [[nodiscard]] auto value() const -> jb::core::JsonValue const& { return _value; }

private:
    explicit QueueEditBaseline(jb::core::JsonValue value);

    jb::core::JsonValue _value;
};

/// Owns minimal protected params and their shared-decoder result for later recovery writing and submission.
struct PreparedQueueUpdate {
    /// Captured UUID/revision and only changed writable fields, encoded as the typed client would send them.
    jb::core::JsonValue          params;
    /// Owning typed request decoded through the same boundary as ordinary request files.
    jb::jobu::UpdateQueueRequest request;
};

/// Prepends queue-specific // instructions to pretty JSON, bounding the complete JSONC draft to 16 MiB.
/// Ordinary request-file and recovery documents still require strict JSON.
[[nodiscard]] auto format_queue_edit_draft(jb::core::JsonValue const& value)
    -> jb::core::Result<std::string, jb::core::Error>;

/// Parses at most 16 MiB of JSONC and validates all retained fields before detecting a no-op.
/// UUID/revision are fixed. Absent top-level fields leave server values unchanged; changed defaults replace the
/// entire partial layer, including removals. Null retention restores inheritance; zero means unlimited retention.
/// Uses existing codecs and registry validation, leaving domain/lifecycle/concurrency admission to the server.
/// Returns an empty optional for a no-op or owning minimal params and a typed update. Neither input nor registry
/// is retained. Performs no I/O or rendering, and errors never contain draft contents. The separate shared compact
/// request serializer enforces the 1 MiB recovery/request-file limit before later persistence and submission.
[[nodiscard]] auto prepare_queue_update(QueueEditBaseline const&           baseline,
                                        std::string_view                   draft,
                                        jb::jobu::AttributeRegistry const& registry)
    -> jb::core::Result<std::optional<PreparedQueueUpdate>, jb::core::Error>;

} // namespace jb::jobuctl::detail
