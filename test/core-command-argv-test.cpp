#include "command_argv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace jb::core;

TEST_CASE("command argv parser owns words and preserves quoted argument boundaries", "[core][argv]")
{
    struct Example {
        std::string_view         input;
        std::vector<std::string> expected;
    };

    auto const examples = std::vector<Example>{
        {.input = "code --wait",                                                     .expected = {"code", "--wait"}                    },
        {.input = " \t\r\n\v\fvi  --clean\t",                                        .expected = {"vi", "--clean"}                     },
        {.input = R"("/opt/editor tools/editor" --wait)",                            .expected = {"/opt/editor tools/editor", "--wait"}},
        {.input = R"(/opt/editor\ tools/editor --wait)",                             .expected = {"/opt/editor tools/editor", "--wait"}},
        {.input = "editor café 'two\xc2\xa0words'",                                  .expected = {"editor", "café", "two\xc2\xa0words"}},
        {.input = R"(editor a"b c"'d e'f)",                                          .expected = {"editor", "ab cd ef"}                },
        {.input = R"(editor '' "" x''y)",                                            .expected = {"editor", "", "", "xy"}              },
        {.input = R"(editor a\ b \' \" \\)",                                         .expected = {"editor", "a b", "'", "\"", "\\"}    },
        {.input = R"(editor 'a\b "$HOME"' "a'b")",                                   .expected = {"editor", R"(a\b "$HOME")", "a'b"}   },
        {.input = R"(editor "a\qb\$c\`d\\e\"f")",                                    .expected = {"editor", R"(a\qb$c`d\e"f)"}         },
        {.input = "editor ab\\\ncd \\\n tail",                                       .expected = {"editor", "abcd", "tail"}            },
        {.input = "editor \"ab\\\ncd\" 'ab\\\ncd'",                                  .expected = {"editor", "abcd", "ab\\\ncd"}        },
        {.input = R"(editor $HOME ${USER} $(id) `id` *.json ~ | > >> < ; && #text)",
         .expected =
             {"editor", "$HOME", "${USER}", "$(id)", "`id`", "*.json", "~", "|", ">", ">>", "<", ";", "&&", "#text"}                   },
    };
    for (auto const& example : examples) {
        CAPTURE(example.input);
        auto arguments = parse_command_argv(example.input);
        REQUIRE(arguments);
        CHECK(*arguments == example.expected);
    }

    auto input     = std::string{"editor --wait"};
    auto arguments = parse_command_argv(input);
    REQUIRE(arguments);
    input.assign("changed");
    CHECK(*arguments == std::vector<std::string>{"editor", "--wait"});
}

TEST_CASE("command argv parser leaves text encoding validation to callers", "[core][argv]")
{
    auto input      = std::string{"editor "};
    input          += static_cast<char>(0xff);
    auto arguments  = parse_command_argv(input);
    REQUIRE(arguments);
    CHECK(*arguments == std::vector<std::string>{"editor", std::string(1, static_cast<char>(0xff))});
}

TEST_CASE("command argv parser rejects malformed configurations without echoing them", "[core][argv]")
{
    auto examples = std::vector<std::string>{"",
                                             " \t\n",
                                             "'' argument",
                                             "\"\" argument",
                                             "\\\n",
                                             "editor 'private-literal",
                                             "editor \"private-literal",
                                             "editor private-literal\\",
                                             "editor \"private-literal\\"};
    examples.push_back(std::string{"editor private-literal"} + '\0' + "suffix");
    for (auto const& input : examples) {
        auto arguments = parse_command_argv(input);
        REQUIRE_FALSE(arguments);
        CHECK(arguments.error().category == jb::core::ErrorCategory::InvalidArgument);
        CHECK(arguments.error().code == "core.command.invalid_argv");
        CHECK(arguments.error().message.find("private-literal") == std::string::npos);
        CHECK(arguments.error().detail.empty());
    }
}
