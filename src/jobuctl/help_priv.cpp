#include "help_priv.hpp"

#include "command_registry_priv.hpp"

#include <fmt/format.h>

#include <span>
#include <string_view>

namespace jb::jobuctl::detail {

namespace {

void append_options(std::string& text, std::span<OptionSpec const> options, bool root)
{
    for (auto const& spec : options) {
        if (!root && spec.option.long_name == "version") {
            continue;
        }
        auto spelling = fmt::format("--{}", spec.option.long_name);
        if (spec.option.short_name != '\0') {
            spelling = fmt::format("-{}, {}", spec.option.short_name, spelling);
        }
        if (!spec.value_name.empty()) {
            spelling += fmt::format(" {}", spec.value_name);
        }
        text += fmt::format("  {}\n      {}\n", spelling, spec.description);
    }
}

void append_actions(std::string& text, std::string_view group)
{
    text += "\nCommands:\n";
    for (auto const& spec : command_specs()) {
        if (spec.group != group) {
            continue;
        }
        text += fmt::format("  {:12} {}", spec.name, spec.summary);
        if (!spec.alias.empty()) {
            text += fmt::format(" (alias: {})", spec.alias);
        }
        text += '\n';
    }
}

} // namespace

auto render_help(HelpCommand const& command) -> std::string
{
    auto        text = std::string{"Usage:\n"};
    auto const* leaf = find_command(command.group, command.action);
    if (leaf) {
        text += fmt::format("  jobuctl [global options] {} {}{}{} [options]\n\n{}\n",
                            leaf->group,
                            leaf->name,
                            leaf->operands.empty() ? "" : " ",
                            leaf->operands,
                            leaf->summary);
        if (!leaf->alias.empty()) {
            text += fmt::format("\nAlias: {} {} is equivalent to {} {}.\n",
                                leaf->group,
                                leaf->alias,
                                leaf->group,
                                leaf->name);
        }
        if (!leaf->rules.empty()) {
            text += fmt::format("\n{}\n", leaf->rules);
        }
        if (!leaf->options.empty()) {
            text += "\nCommand options:\n";
            append_options(text, leaf->options, false);
        }
    }
    else if (!command.group.empty()) {
        text += fmt::format("  jobuctl [global options] {} COMMAND [options]\n", command.group);
        append_actions(text, command.group);
        text += fmt::format("\nUse jobuctl {} COMMAND --help for command details.\n", command.group);
    }
    else {
        text += "  jobuctl [global options] GROUP COMMAND [options]\n"
                "  jobuctl help [GROUP [COMMAND]]\n\nGroups:\n";
        for (auto const& group : command_groups()) {
            text += fmt::format("  {:12} {}\n", group.name, group.summary);
        }
        text += "\nUse jobuctl GROUP --help to list commands, or GROUP COMMAND --help for details.\n";
    }

    text += "\nGlobal options:\n";
    append_options(text, global_options(), command.group.empty());
    text += "\nOptions end at --; subsequent tokens are positional data where accepted.\n";
    if (leaf) {
        text += fmt::format("\nExample:\n  {}\n", leaf->example);
    }
    return text;
}

} // namespace jb::jobuctl::detail
