#pragma once

#include "command_line_priv.hpp"
#include "protocol.hpp"

#include <string_view>

namespace jb::jobuctl::detail {

void print_usage();
void print_operator_error(std::string_view message);
void print_remote_error(jb::rpc::RpcError const& error);
/// Validates a family response before printing the existing human representation.
auto print_command_result(Command const&                             command,
                          jb::core::JsonValue const&                 value,
                          jb::jobu::StandardAttributeRegistry const& registry) -> bool;

} // namespace jb::jobuctl::detail
