#pragma once

#include "attribute_registry.hpp"
#include "history.hpp"
#include "management.hpp"
#include "secret.hpp"
#include "uuid.hpp"

#include <chrono>
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
    JobRunNow,
    RunGet,
    RunList,
    RunCancel,
    AttemptGet,
    AttemptList,
    AttemptOutput,
    SecretSet,
    SecretList,
    SecretDelete,
};

/// Owning requests for the currently registered CLI methods. CommandKind identifies repeated result types.
using CommandRequest = std::variant<std::monostate,
                                    jb::jobu::QueueSelector,
                                    jb::jobu::CreateQueueRequest,
                                    jb::jobu::QueueListRequest,
                                    jb::jobu::UpdateQueueRequest,
                                    jb::core::Uuid,
                                    jb::jobu::CreateJobRequest,
                                    jb::jobu::JobListRequest,
                                    jb::jobu::UpdateJobRequest,
                                    jb::jobu::MoveJobRequest,
                                    jb::jobu::DeleteJobRequest,
                                    jb::jobu::RunNowRequest,
                                    jb::jobu::RunListRequest,
                                    jb::jobu::AttemptKey,
                                    jb::jobu::AttemptListRequest,
                                    jb::jobu::AttemptOutputRequest,
                                    jb::jobu::SetSecretRequest,
                                    jb::jobu::SecretListRequest,
                                    std::string>;

/// Raw secret input is selected during parsing but read only after local help has been resolved.
struct SecretInput {
    enum class Source : std::uint8_t {
        File,
        Stdin
    };

    Source                source;
    std::filesystem::path file;
};

/// Parsed remote work owns its data; method refers only to a static method-name literal.
struct Command {
    std::filesystem::path                socket_path;
    CommandKind                          kind{CommandKind::SystemInfo};
    std::string_view                     method{"system.info"};
    CommandRequest                       request;
    std::optional<std::filesystem::path> request_file;
    std::optional<SecretInput>           secret_input;
    std::chrono::milliseconds            timeout{5000};
    bool                                 json{false};
    bool                                 wait{false};
    bool                                 raw{false};
    std::optional<std::filesystem::path> output_file;
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
    bool                         json_requested{false};
};

/// Selects local help/version or validates remote work without input, socket, or event-loop effects.
/// No views into argv or temporary lexer storage escape in the returned command.
auto parse_command_line(int argc, char* argv[], jb::jobu::StandardAttributeRegistry const& registry) -> ParseResult;

} // namespace jb::jobuctl::detail
