#pragma once

#include "command_line_parser.hpp"
#include "command_line_priv.hpp"
#include "control_client.hpp"
#include "system_info.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace jb::jobuctl::detail {

auto parse_queue_command(std::filesystem::path                          socket_path,
                         std::string_view                               action,
                         std::span<jb::core::CommandLineArgument const> arguments,
                         jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_job_command(std::filesystem::path                          socket_path,
                       std::string_view                               action,
                       std::span<jb::core::CommandLineArgument const> arguments,
                       jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_system_command(std::filesystem::path                          socket_path,
                          std::string_view                               action,
                          std::span<jb::core::CommandLineArgument const> arguments,
                          jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_run_command(std::filesystem::path                          socket_path,
                       std::string_view                               action,
                       std::span<jb::core::CommandLineArgument const> arguments,
                       jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_attempt_command(std::filesystem::path                          socket_path,
                           std::string_view                               action,
                           std::span<jb::core::CommandLineArgument const> arguments,
                           jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_secret_command(std::filesystem::path                          socket_path,
                          std::string_view                               action,
                          std::span<jb::core::CommandLineArgument const> arguments,
                          jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_system_statistics_command(std::filesystem::path                          socket_path,
                                     std::string_view                               action,
                                     std::span<jb::core::CommandLineArgument const> arguments,
                                     jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_queue_statistics_command(std::filesystem::path                          socket_path,
                                    std::string_view                               action,
                                    std::span<jb::core::CommandLineArgument const> arguments,
                                    jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;
auto parse_schedule_command(std::filesystem::path                          socket_path,
                            std::string_view                               action,
                            std::span<jb::core::CommandLineArgument const> arguments,
                            jb::jobu::StandardAttributeRegistry const&     registry) -> CommandBuildResult;

// These names need raw-token protection before the lexical parser consumes their values.
auto is_cli_creation_option(std::string_view name) -> bool;

void print_system_info(jb::jobu::SystemInfo const& info);
/// Escapes terminal control bytes in data embedded in human-readable output.
auto escape_human(std::string_view value) -> std::string;
auto print_queue_result(Command const& command, jb::jobu::ControlReply const& value) -> bool;
auto print_job_result(Command const& command, jb::jobu::ControlReply const& value) -> bool;
auto print_run_result(Command const&                             command,
                      jb::jobu::ControlReply const&              value,
                      jb::jobu::StandardAttributeRegistry const& registry) -> bool;
auto print_attempt_result(Command const& command, jb::jobu::ControlReply const& value) -> bool;
auto print_secret_result(Command const& command, jb::jobu::ControlReply const& value) -> bool;
auto print_statistics_result(jb::jobu::ControlReply const& reply) -> bool;
auto print_schedule_result(CommandKind kind, jb::jobu::ControlReply const& reply) -> bool;

} // namespace jb::jobuctl::detail
