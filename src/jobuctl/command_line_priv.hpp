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
#include <variant>

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

struct CommandBuildResult {
    std::optional<Command> command;
    std::string            error;
};

/// Owning command path; an empty action selects group help, and an empty group selects root help.
struct HelpCommand {
    std::string group;
    std::string action;
};

struct VersionCommand {};

using ParsedCommand = std::variant<HelpCommand, VersionCommand, Command>;

struct ParseResult {
    std::optional<ParsedCommand> action;
    std::string                  error;
    HelpCommand                  usage;
};

/// Selects local help/version or validates remote work without input, socket, or event-loop effects.
/// No views into argv or temporary lexer storage escape in the returned command.
auto parse_command_line(int argc, char* argv[], jb::jobu::StandardAttributeRegistry const& registry) -> ParseResult;

} // namespace jb::jobuctl::detail
