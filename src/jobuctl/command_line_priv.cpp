#include "command_line_priv.hpp"

#include "command_helpers_priv.hpp"
#include "command_line_parser.hpp"
#include "commands/commands_priv.hpp"

#include <array>
#include <span>
#include <utility>
#include <vector>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

constexpr std::array command_line_options{
    CommandLineOption{.long_name = "socket", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "weight", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "concurrency-limit", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "recovery-policy", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "idempotency-key", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "id", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "name", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "include-deleted"},
    CommandLineOption{.long_name = "limit", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "after", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "new-name", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "queue-id", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "queue-name", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "type", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "at", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "command", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "arg", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "working-directory", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "env", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "unset-env", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "expected-exit-code", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "url", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "method", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "priority", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "revision", .value_mode = CommandLineValueMode::Required},
    CommandLineOption{.long_name = "clear-name"},
};

auto preserve_cli_argument_tokens(int argc, char* argv[]) -> std::vector<std::string>
{
    // These option names used to be unknown and could follow --arg literally. Protect that one raw token before
    // descriptor parsing can recognize it as an option and consume the token after it as its own value.
    auto tokens = std::vector<std::string>{};
    tokens.reserve(static_cast<std::size_t>(argc));
    auto positional_only = false;
    for (auto index = 0; index < argc; ++index) {
        auto const token = std::string_view{argv[index]};
        if (!positional_only && token == "--arg" && index + 1 < argc) {
            auto const next = std::string_view{argv[index + 1]};
            if (next.starts_with("--")) {
                auto const name = next.substr(2);
                if (is_cli_creation_option(name.substr(0, name.find('=')))) {
                    tokens.push_back("--arg=" + std::string{next});
                    ++index;
                    continue;
                }
            }
        }
        positional_only = positional_only || token == "--";
        tokens.emplace_back(token);
    }
    return tokens;
}

} // namespace

auto parse_command_line(int argc, char* argv[], StandardAttributeRegistry const& registry) -> ParseResult
{
    // Keep normalized token storage alive through family validation, then return only owning command data.
    auto tokens          = preserve_cli_argument_tokens(argc, argv);
    auto normalized_argv = std::vector<char const*>{};
    normalized_argv.reserve(tokens.size());
    for (auto const& token : tokens) {
        normalized_argv.push_back(token.c_str());
    }

    CommandLineParser parser{static_cast<int>(normalized_argv.size()), normalized_argv.data(), command_line_options};
    auto const&       arguments = parser.arguments();
    if (arguments.size() < 3U) {
        return parse_failure("expected --socket PATH followed by a command");
    }

    auto const socket = option_value(arguments[0]);
    if (!socket || arguments[0].name() != "socket") {
        return parse_failure("--socket PATH must be the first argument");
    }
    auto socket_path = std::filesystem::path{std::string{*socket}};

    if (arguments[1].kind() != CommandLineArgumentKind::Positional ||
        arguments[2].kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("expected a command family and action");
    }

    auto const family    = arguments[1].token();
    auto const action    = arguments[2].token();
    auto       remaining = std::span<CommandLineArgument const>{arguments}.subspan(3);
    if (family == "system") {
        return parse_system_command(std::move(socket_path), action, remaining);
    }
    if (family == "queue") {
        return parse_queue_command(std::move(socket_path), action, remaining, registry);
    }
    if (family == "job") {
        return parse_job_command(std::move(socket_path), action, remaining, registry);
    }
    return parse_failure("unknown command");
}

} // namespace jb::jobuctl::detail
