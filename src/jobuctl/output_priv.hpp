#pragma once

#include "command_line_priv.hpp"
#include "control_client.hpp"
#include "error.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace jb::jobuctl::detail {

/// Safe, owning command-line error ready for human or machine rendering.
struct CliError {
    std::string                 kind{"local"};
    std::string                 code;
    std::optional<std::int64_t> rpc_code;
    std::optional<std::string>  category;
    std::string                 message;
    bool                        outcome_unknown{false};
};

auto local_error(jb::core::Error const& error, bool outcome_unknown = false) -> CliError;
auto remote_error(jb::rpc::RpcError const& error) -> CliError;
void print_error(bool json, CliError const& error);

/// Prints one typed result. JSON output is prepared in full before writing to stdout.
auto print_command_result(Command const&                             command,
                          jb::jobu::ControlReply const&              reply,
                          jb::jobu::StandardAttributeRegistry const& registry) -> bool;

/// Writes exactly one decoded chunk as bytes. A file is created exclusively and partial files are removed on failure.
[[nodiscard]] auto write_output_chunk(Command const& command, jb::jobu::AttemptOutputChunk const& chunk)
    -> jb::core::Result<void, jb::core::Error>;

} // namespace jb::jobuctl::detail
