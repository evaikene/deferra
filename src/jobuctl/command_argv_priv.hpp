#pragma once

#include "error.hpp"
#include "result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace jb::jobuctl::detail {

/// Splits a command into owning argv values, including the executable in the first element.
/// ASCII whitespace separates words outside quotes; adjacent quoted/unquoted segments form one word.
/// Single quotes preserve every byte. Outside quotes, backslash escapes the following byte.
/// In double quotes it escapes only double quote, backslash, dollar, backtick, or newline; otherwise it stays literal.
/// Backslash-newline is removed outside single quotes. Quoted empty arguments after the executable are preserved.
/// Rejects NUL, unmatched quotes, trailing escapes, and an absent/empty executable, without echoing input.
/// Performs no expansion, environment lookup, shell evaluation, or execution; shell operators are ordinary bytes.
[[nodiscard]] auto parse_command_argv(std::string_view command)
    -> jb::core::Result<std::vector<std::string>, jb::core::Error>;

} // namespace jb::jobuctl::detail
