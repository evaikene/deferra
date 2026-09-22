#pragma once

#include "command_line_parser.hpp"
#include "command_line_priv.hpp"
#include "system_info.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace jb::jobuctl::detail {

auto parse_queue_command(std::filesystem::path                          socket_path,
                         std::string_view                               action,
                         std::span<jb::core::CommandLineArgument const> arguments,
                         jb::jobu::StandardAttributeRegistry const&     registry) -> ParseResult;
auto parse_job_command(std::filesystem::path                          socket_path,
                       std::string_view                               action,
                       std::span<jb::core::CommandLineArgument const> arguments,
                       jb::jobu::StandardAttributeRegistry const&     registry) -> ParseResult;
auto parse_system_command(std::filesystem::path                          socket_path,
                          std::string_view                               action,
                          std::span<jb::core::CommandLineArgument const> arguments) -> ParseResult;

// These names need raw-token protection before the lexical parser consumes their values.
auto is_cli_creation_option(std::string_view name) -> bool;

void print_system_info(jb::jobu::SystemInfo const& info);
auto print_queue_result(Command const&                             command,
                        jb::core::JsonValue const&                 value,
                        jb::jobu::StandardAttributeRegistry const& registry) -> bool;
auto print_job_result(Command const&                             command,
                      jb::core::JsonValue const&                 value,
                      jb::jobu::StandardAttributeRegistry const& registry) -> bool;

} // namespace jb::jobuctl::detail
