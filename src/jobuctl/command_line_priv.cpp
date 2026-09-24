#include "command_line_priv.hpp"

#include "command_helpers_priv.hpp"
#include "command_line_parser.hpp"
#include "command_registry_priv.hpp"
#include "commands/commands_priv.hpp"
#include "event_loop_types.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto is_cli_argument_value(std::string_view token, std::span<CommandLineOption const> options) -> bool
{
    if (token == "--") {
        return false;
    }
    if (!token.starts_with("--")) {
        return true;
    }
    auto const spelling = token.substr(2);
    auto const name     = spelling.substr(0, spelling.find('='));
    return is_cli_creation_option(name) || name == "help" || name == "version" || name == "json" || name == "timeout" ||
           name == "request-file" || std::ranges::find(options, name, &CommandLineOption::long_name) == options.end();
}

auto preserve_value_tokens(int argc, char* argv[], std::span<CommandLineOption const> options)
    -> std::vector<std::string>
{
    // --arg historically accepts unknown dash-leading tokens, including what are now help/version options.
    // Attach that one value before lexing so a new short-option descriptor cannot split it into actions.
    // Existing recognized options still signal a missing argument; -- always remains a terminator.
    auto tokens = std::vector<std::string>{};
    tokens.reserve(static_cast<std::size_t>(argc));
    auto positional_only = false;
    for (auto index = 0; index < argc; ++index) {
        auto const token = std::string_view{argv[index]};
        if (!positional_only && index > 0 && index + 1 < argc) {
            auto const next = std::string_view{argv[index + 1]};
            auto const signed_priority =
                token == "--priority" && next.size() > 1 && next[0] == '-' && next[1] >= '0' && next[1] <= '9';
            if ((token == "--arg" && is_cli_argument_value(next, options)) || signed_priority) {
                tokens.push_back(std::string{token} + "=" + std::string{next});
                ++index;
                continue;
            }
        }
        positional_only = positional_only || token == "--";
        tokens.emplace_back(token);
    }
    return tokens;
}

struct Selection {
    HelpCommand                      usage;
    CommandSpec const*               command{nullptr};
    std::vector<CommandLineArgument> remaining;
    bool                             help_path{false};
    std::string                      error;
};

auto select_path(std::span<CommandLineArgument const> arguments) -> Selection
{
    auto selected        = Selection{};
    auto positional_only = false;
    for (auto const& argument : arguments) {
        if (argument.kind() == CommandLineArgumentKind::Terminator) {
            positional_only = true;
            continue;
        }
        if (argument.kind() != CommandLineArgumentKind::Positional || positional_only || selected.command) {
            selected.remaining.push_back(argument);
            continue;
        }

        auto const token = argument.token();
        if (selected.usage.group.empty()) {
            if (token == "help" && !selected.help_path) {
                selected.help_path = true;
                continue;
            }
            if (!find_group(token)) {
                selected.error = "unknown command group";
                return selected;
            }
            selected.usage.group = token;
            continue;
        }
        selected.command = find_command(selected.usage.group, token);
        if (!selected.command) {
            selected.error = "unknown command action";
            return selected;
        }
        selected.usage.action = token;
    }
    return selected;
}

struct ParsedOptions {
    std::optional<std::filesystem::path> socket;
    std::optional<std::filesystem::path> request_file;
    std::vector<CommandLineArgument>     local;
    std::chrono::milliseconds            timeout{5000};
    bool                                 json{false};
    bool                                 help{false};
    bool                                 version{false};
    std::string                          error;
};

auto parse_options(Selection const& selected) -> ParsedOptions
{
    auto parsed   = ParsedOptions{};
    auto seen     = std::vector<std::string_view>{};
    auto operands = std::size_t{0};
    for (auto const& argument : selected.remaining) {
        if (argument.kind() == CommandLineArgumentKind::Positional) {
            ++operands;
            parsed.local.push_back(argument);
            continue;
        }

        // Validate lexical shape even for help. Domain values and required operands are checked only for remote work.
        auto const* spec   = find_option(global_options(), argument.name());
        auto const  global = spec != nullptr;
        if (!spec && selected.command && !selected.help_path) {
            spec = find_option(selected.command->options, argument.name());
        }
        if (argument.kind() != CommandLineArgumentKind::Option || !spec) {
            parsed.error = "unknown option for this command";
            return parsed;
        }
        if (argument.missing_value()) {
            parsed.error = "--" + std::string{spec->option.long_name} + " requires a value";
            return parsed;
        }
        if (spec->option.value_mode == CommandLineValueMode::None && argument.has_value()) {
            parsed.error = "--" + std::string{spec->option.long_name} + " does not accept a value";
            return parsed;
        }
        if (!spec->repeatable && std::ranges::find(seen, argument.name()) != seen.end()) {
            parsed.error = "--" + std::string{spec->option.long_name} + " may be supplied only once";
            return parsed;
        }
        seen.push_back(argument.name());

        if (!global) {
            parsed.local.push_back(argument);
        }
        else if (argument.name() == "socket") {
            if (argument.value()->empty()) {
                parsed.error = "--socket requires a nonempty path";
                return parsed;
            }
            parsed.socket = std::filesystem::path{std::string{*argument.value()}};
        }
        else if (argument.name() == "help") {
            parsed.help = true;
        }
        else if (argument.name() == "json") {
            parsed.json = true;
        }
        else if (argument.name() == "timeout") {
            auto const available =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - Clock::now());
            auto const maximum = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(available.count()),
                static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()));
            auto const timeout = parse_unsigned(*argument.value(), 1, maximum);
            if (!timeout) {
                parsed.error = "--timeout must be a positive millisecond count";
                return parsed;
            }
            parsed.timeout = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(*timeout)};
        }
        else if (argument.name() == "request-file") {
            if (argument.value()->empty()) {
                parsed.error = "--request-file requires a nonempty path or -";
                return parsed;
            }
            parsed.request_file = std::filesystem::path{std::string{*argument.value()}};
        }
        else if (argument.name() == "version") {
            parsed.version = true;
        }
    }

    auto const maximum =
        selected.command && !selected.help_path && !parsed.request_file ? selected.command->maximum_operands : 0;
    if (operands > maximum) {
        parsed.error = "unexpected extra operand";
    }
    else if (parsed.version && (!selected.usage.group.empty() || selected.help_path || parsed.help)) {
        parsed.error = "--version must be used at root without help";
    }
    return parsed;
}

} // namespace

auto parse_command_line(int argc, char* argv[], StandardAttributeRegistry const& registry) -> ParseResult
{
    // All views below remain inside this call; local actions and built requests own the data that escapes it.
    auto const descriptors     = lexical_options();
    auto const tokens          = preserve_value_tokens(argc, argv, descriptors);
    auto       normalized_argv = std::vector<char const*>{};
    normalized_argv.reserve(tokens.size());
    for (auto const& token : tokens) {
        normalized_argv.push_back(token.c_str());
    }
    CommandLineParser parser{static_cast<int>(normalized_argv.size()), normalized_argv.data(), descriptors};
    auto const        json_requested = std::ranges::any_of(parser.arguments(), [](CommandLineArgument const& argument) {
        return argument.kind() == CommandLineArgumentKind::Option && argument.name() == "json";
    });
    auto              selected       = select_path(parser.arguments());
    if (!selected.error.empty()) {
        return {.error          = std::move(selected.error),
                .usage          = std::move(selected.usage),
                .json_requested = json_requested};
    }
    auto options = parse_options(selected);
    if (!options.error.empty()) {
        return {.error          = std::move(options.error),
                .usage          = std::move(selected.usage),
                .json_requested = json_requested};
    }

    // Help/version exit before request building, selector validation, or construction of runtime infrastructure.
    if (options.version) {
        return {.action = VersionCommand{}};
    }
    if (options.help || selected.help_path || !selected.command) {
        return {.action = std::move(selected.usage)};
    }
    if (!options.socket) {
        return {.error          = "--socket PATH is required for remote commands",
                .usage          = std::move(selected.usage),
                .json_requested = json_requested};
    }
    if (options.request_file) {
        if (!options.local.empty()) {
            return {.error          = "--request-file cannot be combined with command operands or options",
                    .usage          = std::move(selected.usage),
                    .json_requested = json_requested};
        }
        return {
            .action = Command{
                              .socket_path  = std::move(*options.socket),
                              .kind         = selected.command->kind,
                              .method       = selected.command->capability,
                              .request_file = std::move(options.request_file),
                              .timeout      = options.timeout,
                              .json         = options.json,
                              }
        };
    }

    auto built = selected.command->build(std::move(*options.socket), selected.command->name, options.local, registry);
    if (!built.command) {
        return {.error = std::move(built.error), .usage = std::move(selected.usage), .json_requested = json_requested};
    }
    // The registry's canonical capability is also the wire method, including for aliases.
    built.command->kind    = selected.command->kind;
    built.command->method  = selected.command->capability;
    built.command->timeout = options.timeout;
    built.command->json    = options.json;
    return {.action = std::move(*built.command)};
}

} // namespace jb::jobuctl::detail
