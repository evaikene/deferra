#include "command_argv_priv.hpp"

#include <cstdint>
#include <utility>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;
using Arguments = Result<std::vector<std::string>, Error>;

enum class Quote : std::uint8_t {
    None,
    Single,
    Double
};

auto invalid_command(char const* message) -> Arguments
{
    return Arguments::failure(
        {.category = ErrorCategory::InvalidArgument, .code = "jobuctl.command.invalid_argv", .message = message});
}

auto whitespace(char value) -> bool
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

auto double_quote_escape(char value) -> bool
{
    return value == '"' || value == '\\' || value == '$' || value == '`' || value == '\n';
}

} // namespace

auto parse_command_argv(std::string_view command) -> Arguments
{
    if (command.find('\0') != std::string_view::npos) {
        return invalid_command("Command arguments cannot contain NUL");
    }

    auto arguments    = std::vector<std::string>{};
    auto word         = std::string{};
    auto quote        = Quote::None;
    // A quoted empty word is an argument, while whitespace or a line continuation alone is not.
    auto word_started = false;

    for (auto index = std::size_t{0}; index < command.size(); ++index) {
        auto const value = command[index];
        if (quote == Quote::Single) {
            if (value == '\'') {
                quote = Quote::None;
            }
            else {
                word += value;
            }
            continue;
        }

        if (value == '\\') {
            if (index + 1 == command.size()) {
                return invalid_command("Command ends with an incomplete backslash escape");
            }
            auto const next = command[index + 1];
            if (quote == Quote::Double && !double_quote_escape(next)) {
                word += value;
            }
            else {
                ++index;
                if (next != '\n') {
                    word         += next;
                    word_started  = true;
                }
            }
            continue;
        }

        if (quote == Quote::Double) {
            if (value == '"') {
                quote = Quote::None;
            }
            else {
                word += value;
            }
            continue;
        }

        if (value == '\'' || value == '"') {
            quote        = value == '\'' ? Quote::Single : Quote::Double;
            word_started = true;
        }
        else if (whitespace(value)) {
            if (word_started) {
                arguments.push_back(std::move(word));
                word.clear();
                word_started = false;
            }
        }
        else {
            word         += value;
            word_started  = true;
        }
    }

    if (quote != Quote::None) {
        return invalid_command("Command has an unterminated quote");
    }
    if (word_started) {
        arguments.push_back(std::move(word));
    }
    if (arguments.empty() || arguments.front().empty()) {
        return invalid_command("Command requires a nonempty executable");
    }
    return Arguments::success(std::move(arguments));
}

} // namespace jb::jobuctl::detail
