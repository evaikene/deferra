#pragma once

#include "command_line_parser.hpp"
#include "command_line_priv.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace jb::jobuctl::detail {

struct OptionSpec {
    jb::core::CommandLineOption option;
    std::string_view            value_name;
    std::string_view            description;
    bool                        repeatable{false};
};

using CommandBuilder = auto (*)(std::filesystem::path,
                                std::string_view,
                                std::span<jb::core::CommandLineArgument const>,
                                jb::jobu::StandardAttributeRegistry const&) -> CommandBuildResult;

/// Static metadata shared by syntax selection, help, and canonical request dispatch.
/// Builders retain responsibility for domain values and mutually dependent fields.
struct CommandSpec {
    std::string_view            group;
    std::string_view            name;
    CommandKind                 kind;
    std::string_view            alias;
    std::string_view            summary;
    std::string_view            operands;
    std::size_t                 maximum_operands;
    std::span<OptionSpec const> options;
    std::string_view            rules;
    std::string_view            example;
    std::string_view            capability;
    CommandBuilder              build;
};

struct GroupSpec {
    std::string_view name;
    std::string_view summary;
};

auto command_groups() -> std::span<GroupSpec const>;
auto command_specs() -> std::span<CommandSpec const>;
auto global_options() -> std::span<OptionSpec const>;
auto find_group(std::string_view name) -> GroupSpec const*;
auto find_command(std::string_view group, std::string_view action) -> CommandSpec const*;
auto find_option(std::span<OptionSpec const> options, std::string_view name) -> OptionSpec const*;
/// Returns deduplicated descriptors whose views borrow only static registry storage.
auto lexical_options() -> std::vector<jb::core::CommandLineOption>;

} // namespace jb::jobuctl::detail
