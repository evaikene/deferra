#include "editor_command_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace jb::jobuctl::detail;

TEST_CASE("editor command resolution uses explicit inputs and fixed precedence", "[jobuctl][editor]")
{
    struct Example {
        EditorCommandInputs      inputs;
        std::vector<std::string> expected;
    };

    auto const examples = std::vector<Example>{
        {.inputs   = {.override_command = "override --wait", .visual = "visual", .editor = "editor"},
         .expected = {"override", "--wait"}                                                                                                 },
        {.inputs = {.visual = "visual --wait", .editor = "editor"},                                   .expected = {"visual", "--wait"}      },
        {.inputs = {.editor = "editor --foreground"},                                                 .expected = {"editor", "--foreground"}},
        {.inputs = {},                                                                                .expected = {"vi"}                    },
        {.inputs   = {.override_command = R"("/editor tools/tool" --wait '')"},
         .expected = {"/editor tools/tool", "--wait", ""}                                                                                   },
        {.inputs = {.override_command = "valid", .visual = "'invalid", .editor = "'invalid"},         .expected = {"valid"}                 },
        {.inputs = {.visual = "valid", .editor = "'invalid"},                                         .expected = {"valid"}                 },
    };
    for (auto const& example : examples) {
        auto arguments = resolve_editor_command(example.inputs);
        REQUIRE(arguments);
        CHECK(*arguments == example.expected);
    }
}

TEST_CASE("invalid selected editor configuration fails without fallback or literal disclosure", "[jobuctl][editor]")
{
    struct Example {
        EditorCommandInputs inputs;
        std::string_view    source;
    };

    auto const examples = std::vector<Example>{
        {.inputs = {.override_command = "", .visual = "valid", .editor = "valid"}, .source = "explicit"},
        {.inputs = {.override_command = " ", .visual = "valid"},                   .source = "explicit"},
        {.inputs = {.override_command = "'private-literal", .visual = "valid"},    .source = "explicit"},
        {.inputs = {.visual = " ", .editor = "valid"},                             .source = "VISUAL"  },
        {.inputs = {.visual = "'private-literal", .editor = "valid"},              .source = "VISUAL"  },
        {.inputs = {.editor = "'private-literal"},                                 .source = "EDITOR"  },
    };
    for (auto const& example : examples) {
        auto arguments = resolve_editor_command(example.inputs);
        REQUIRE_FALSE(arguments);
        CHECK(arguments.error().code == "jobuctl.editor.invalid_command");
        CHECK(arguments.error().message.find(example.source) != std::string::npos);
        CHECK(arguments.error().message.find("private-literal") == std::string::npos);
    }
}
