#pragma once

#include "command_line_parser.hpp"
#include "management.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace jb::jobuctl::detail {

enum class QueueSelectionForm : std::uint8_t {
    InferredId,
    ExplicitId,
    ExactName,
};

/// Owning CLI intent for later resolution; inferred IDs retain the exact original name-probe bytes.
struct QueueSelectorIntent {
    QueueSelectionForm form;
    std::string        original_text;
};

struct ParsedQueueSelector {
    jb::jobu::QueueSelector selector;
    QueueSelectorIntent     intent;
};

struct QueueSelectorParseResult {
    std::optional<ParsedQueueSelector> selector;
    std::string                        error;
};

/// Identifies positional selection and the explicit --id/--name aliases, never replacement fields.
auto is_queue_selector_argument(jb::core::CommandLineArgument const& argument) -> bool;

/// Parses at most one selector among registry-validated arguments; other command options are ignored.
/// No selector and no error is valid for cursor-only stats; other callers must require a selector.
/// Raw request-file commands bypass this helper entirely. No input views escape the returned value.
auto parse_queue_selector(std::span<jb::core::CommandLineArgument const> arguments) -> QueueSelectorParseResult;

} // namespace jb::jobuctl::detail
