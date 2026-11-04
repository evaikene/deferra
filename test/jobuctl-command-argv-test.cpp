#include "command_argv.hpp"
#include "edit_recovery_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace jb::jobuctl::detail;

TEST_CASE("command argv parser round trips recovery shell quoting without executing it", "[jobuctl][argv]")
{
    auto const arguments = std::vector<std::string>{"/editor path/tool",
                                                    "",
                                                    "one'two",
                                                    "\"quoted\"",
                                                    "a\\b",
                                                    "$HOME;$(id)|*",
                                                    "line\nbreak",
                                                    "tab\tvalue"};
    auto       quoted    = quote_shell_argv(arguments);
    REQUIRE(quoted);
    auto parsed = jb::core::parse_command_argv(*quoted);
    REQUIRE(parsed);
    CHECK(*parsed == arguments);
}
