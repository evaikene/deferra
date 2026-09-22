#pragma once

#include "attribute_registry.hpp"
#include "json.hpp"
#include "management.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace jb::jobuctl::detail {

enum class CommandKind : std::uint8_t {
    SystemInfo,
    QueueCreate,
    QueueGet,
    QueueList,
    QueueUpdate,
    QueueSuspend,
    QueueResume,
    QueueDelete,
    JobCreate,
    JobGet,
    JobList,
    JobUpdate,
    JobSuspend,
    JobResume,
    JobMove,
    JobDelete,
};

/// Parsed remote work owns its data; method refers only to a static method-name literal.
struct Command {
    std::filesystem::path                  socket_path;
    CommandKind                            kind{CommandKind::SystemInfo};
    std::string_view                       method{"system.info"};
    std::optional<jb::core::JsonValue>     params;
    std::optional<jb::jobu::QueueSelector> selector;
    std::optional<jb::core::Uuid>          job_id;
};

struct ParseResult {
    std::optional<Command> command;
    std::string            error;
};

/// Parses and validates existing commands without socket or event-loop effects.
/// No views into argv or temporary lexer storage escape in the returned command.
auto parse_command_line(int argc, char* argv[], jb::jobu::StandardAttributeRegistry const& registry) -> ParseResult;

} // namespace jb::jobuctl::detail
