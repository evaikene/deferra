#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "secret_json.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout macro
#include <optional>
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto parse_set(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    auto name  = std::optional<std::string>{};
    auto input = std::optional<SecretInput>{};
    for (auto const& argument : arguments) {
        if (argument.kind() == CommandLineArgumentKind::Positional) {
            if (name || argument.token().empty()) {
                return parse_failure("secret set requires one name and exactly one input source");
            }
            name = argument.token();
        }
        else if (argument.name() == "file") {
            auto path = option_value(argument);
            if (input || !path) {
                return parse_failure("secret set requires exactly one of --file or --stdin");
            }
            input = SecretInput{.source = SecretInput::Source::File, .file = std::filesystem::path{*path}};
        }
        else if (argument.name() == "stdin") {
            if (input) {
                return parse_failure("secret set requires exactly one of --file or --stdin");
            }
            input = SecretInput{.source = SecretInput::Source::Stdin};
        }
    }
    if (!name || !input) {
        return parse_failure("secret set requires one name and exactly one of --file or --stdin");
    }

    return {
        .command = Command{.socket_path  = std::move(socket_path),
                           .kind         = CommandKind::SecretSet,
                           .request      = SetSecretRequest{.name = std::move(*name)},
                           .secret_input = std::move(input)}
    };
}

auto parse_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    auto request = SecretListRequest{};
    for (auto const& argument : arguments) {
        auto value = option_value(argument);
        if (!value) {
            return parse_failure("secret list requires nonempty option values");
        }
        if (argument.name() == "limit") {
            auto limit = parse_unsigned(*value, 1, 200);
            if (!limit) {
                return parse_failure("--limit must be an integer from 1 through 200");
            }
            request.limit = static_cast<std::size_t>(*limit);
        }
        else if (argument.name() == "after-name") {
            request.after_name = *value;
        }
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::SecretList,
                           .request     = std::move(request)}
    };
}

auto parse_delete(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    if (arguments.size() != 1U || arguments.front().kind() != CommandLineArgumentKind::Positional ||
        arguments.front().token().empty()) {
        return parse_failure("secret delete requires one name");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::SecretDelete,
                           .request     = std::string{arguments.front().token()}}
    };
}

auto metadata_line(SecretMetadata const& metadata) -> std::optional<std::string>
{
    auto encoded = secret_metadata_to_json(metadata);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields = encoded->as_object();
    return fmt::format("Secret {}: created_at={}, updated_at={}\n",
                       escape_human(fields.at("name").as_string()),
                       fields.at("created_at").as_string(),
                       fields.at("updated_at").as_string());
}

} // namespace

auto parse_secret_command(std::filesystem::path                socket_path,
                          std::string_view                     action,
                          std::span<CommandLineArgument const> arguments,
                          StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    static_cast<void>(registry);
    if (action == "set") {
        return parse_set(std::move(socket_path), arguments);
    }
    if (action == "list") {
        return parse_list(std::move(socket_path), arguments);
    }
    if (action == "delete") {
        return parse_delete(std::move(socket_path), arguments);
    }
    return parse_failure("unknown secret action");
}

auto print_secret_result(Command const& command, ControlReply const& value) -> bool
{
    if (command.kind == CommandKind::SecretDelete) {
        if (!std::holds_alternative<EmptyReply>(value)) {
            return false;
        }
        fmt::print(stdout, "Secret deleted\n");
        return true;
    }
    if (command.kind == CommandKind::SecretSet) {
        auto const* metadata = std::get_if<SecretMetadata>(&value);
        auto        line     = metadata ? metadata_line(*metadata) : std::nullopt;
        if (!line) {
            return false;
        }
        fmt::print(stdout, "{}", *line);
        return true;
    }
    auto const* page = std::get_if<SecretPage>(&value);
    if (!page) {
        return false;
    }
    auto text = std::string{};
    for (auto const& metadata : page->items) {
        auto line = metadata_line(metadata);
        if (!line) {
            return false;
        }
        text += *line;
    }
    if (page->items.empty()) {
        text = "No secrets\n";
    }
    if (page->next_after_name) {
        text += fmt::format("Next after name: {}\n", escape_human(*page->next_after_name));
    }
    fmt::print(stdout, "{}", text);
    return true;
}

} // namespace jb::jobuctl::detail
