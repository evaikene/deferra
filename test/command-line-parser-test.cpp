#include "command_line_parser.hpp"

#include <array>
#include <chrono> // IWYU pragma: keep for std::chrono_literals

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;
using namespace std::chrono_literals;

// NOLINTBEGIN(readability-magic-numbers)

TEST_CASE("Command line parser iterates arguments in command-line order", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "verbose", .short_name = 'v'},
        CommandLineOption{.long_name = "output", .short_name = 'o', .value_mode = CommandLineValueMode::Required},
    };
    char const* argv[] = {"program", "input.txt", "--verbose", "-o", "out.txt", "--unknown", nullptr};

    CommandLineParser parser{6, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);

    CHECK(args[0].kind() == CommandLineArgumentKind::Positional);
    CHECK(args[0].token() == "input.txt");

    CHECK(args[1].kind() == CommandLineArgumentKind::Option);
    CHECK(args[1].known());
    CHECK(args[1].name() == "verbose");

    CHECK(args[2].kind() == CommandLineArgumentKind::Option);
    CHECK(args[2].known());
    CHECK(args[2].name() == "output");
    CHECK(args[2].short_name() == 'o');
    REQUIRE(args[2].value());
    CHECK(*args[2].value() == "out.txt");

    CHECK_FALSE(args[3].known());
    CHECK(args[3].kind() == CommandLineArgumentKind::Unknown);
    CHECK(args[3].name() == "unknown");
}

TEST_CASE("Command line parser supports range-for iteration", "[core][command-line]")
{
    char const* argv[] = {"program", "one", "two", "three", nullptr};

    CommandLineParser parser{4, argv};

    auto count = 0;
    for (auto const& arg : parser) {
        CHECK(arg.kind() == CommandLineArgumentKind::Positional);
        ++count;
    }

    CHECK(count == 3);
}

TEST_CASE("Command line parser returns program basename", "[core][command-line]")
{
    SECTION("POSIX path")
    {
        char const* argv[] = {"/usr/bin/myapp", nullptr};

        CommandLineParser parser{1, argv};

        CHECK(parser.program_name() == "myapp");
    }

    SECTION("Windows path")
    {
        char const* argv[] = {"C:\\Program Files\\myapp.exe", nullptr};

        CommandLineParser parser{1, argv};

        CHECK(parser.program_name() == "myapp.exe");
    }

    SECTION("no path")
    {
        char const* argv[] = {"myapp", nullptr};

        CommandLineParser parser{1, argv};

        CHECK(parser.program_name() == "myapp");
    }

    SECTION("empty argv")
    {
        CommandLineParser parser{0, nullptr};

        CHECK(parser.program_name().empty());
    }

    SECTION("empty argv zero")
    {
        char const* argv[] = {"", nullptr};

        CommandLineParser parser{1, argv};

        CHECK(parser.program_name().empty());
    }
}

TEST_CASE("Command line parser keeps unknown options in the argument stream", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "known", .short_name = 'k'},
    };
    char const* argv[] = {"program", "--known", "--missing", "-x", nullptr};

    CommandLineParser parser{4, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 3);
    CHECK(args[0].known());
    CHECK_FALSE(args[1].known());
    CHECK(args[1].kind() == CommandLineArgumentKind::Unknown);
    CHECK(args[1].name() == "missing");
    CHECK_FALSE(args[2].known());
    CHECK(args[2].kind() == CommandLineArgumentKind::Unknown);
    CHECK(args[2].short_name() == 'x');
}

TEST_CASE("Command line parser parses inline and next-token values", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "output", .short_name = 'o', .value_mode = CommandLineValueMode::Required},
    };
    char const* argv[] = {"program", "--output=first.txt", "-osecond.txt", "--output", "third.txt", nullptr};

    CommandLineParser parser{5, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 3);

    REQUIRE(args[0].value());
    CHECK(*args[0].value() == "first.txt");
    CHECK(args[0].value_is_inline());

    REQUIRE(args[1].value());
    CHECK(*args[1].value() == "second.txt");
    CHECK(args[1].value_is_inline());

    REQUIRE(args[2].value());
    CHECK(*args[2].value() == "third.txt");
    CHECK_FALSE(args[2].value_is_inline());
}

TEST_CASE("Command line parser reports missing values on parsed arguments", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "output", .short_name = 'o', .value_mode = CommandLineValueMode::Required},
    };
    char const* argv[] = {"program", "--output=", "--output", "-o", nullptr};

    CommandLineParser parser{4, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 3);

    CHECK(args[0].has_value());
    REQUIRE(args[0].value());
    CHECK(args[0].value()->empty());
    CHECK_FALSE(args[0].missing_value());

    CHECK_FALSE(args[1].has_value());
    CHECK(args[1].missing_value());

    CHECK_FALSE(args[2].has_value());
    CHECK(args[2].missing_value());
}

TEST_CASE("Command line argument converts typed option values", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "enabled", .short_name = 'e'},
        CommandLineOption{.long_name = "count", .short_name = 'c', .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "ratio", .short_name = 'r', .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "timeout", .short_name = 't', .value_mode = CommandLineValueMode::Required},
    };
    char const* argv[] = {"program", "--enabled", "--count", "42", "--ratio=1.25", "-t5s", nullptr};

    CommandLineParser parser{6, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);

    auto enabled = args[0].boolean_value();
    REQUIRE(enabled);
    CHECK(*enabled.value);

    auto count = args[1].integer_value();
    REQUIRE(count);
    CHECK(*count.value == 42);

    auto ratio = args[2].floating_point_value();
    REQUIRE(ratio);
    CHECK(*ratio.value == 1.25);

    auto timeout = args[3].duration_value();
    REQUIRE(timeout);
    CHECK(*timeout.value == 5s);
}

TEST_CASE("Command line argument converts positional values", "[core][command-line]")
{
    char const* argv[] = {"program", "17", "2.5", "3m", "yes", nullptr};

    CommandLineParser parser{5, argv};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);

    auto integer = args[0].integer_value();
    REQUIRE(integer);
    CHECK(*integer.value == 17);

    auto floating_point = args[1].floating_point_value();
    REQUIRE(floating_point);
    CHECK(*floating_point.value == 2.5);

    auto duration = args[2].duration_value();
    REQUIRE(duration);
    CHECK(*duration.value == 180s);

    auto boolean = args[3].boolean_value();
    REQUIRE(boolean);
    CHECK(*boolean.value);
}

TEST_CASE("Command line argument typed conversions report missing and invalid values", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "enabled", .short_name = 'e', .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "count",   .short_name = 'c', .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "ratio",   .short_name = 'r', .value_mode = CommandLineValueMode::Required},
        CommandLineOption{.long_name = "timeout", .short_name = 't', .value_mode = CommandLineValueMode::Required},
    };
    char const* argv[] = {"program", "--enabled", "--count", "--ratio=", "--timeout=soon", nullptr};

    CommandLineParser parser{5, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);

    auto missing_boolean = args[0].boolean_value();
    CHECK_FALSE(missing_boolean);
    CHECK(missing_boolean.error == "missing boolean value for argument: '--enabled'");

    auto missing = args[1].integer_value();
    CHECK_FALSE(missing);
    CHECK(missing.error == "missing integer value for argument: '--count'");

    auto empty = args[2].floating_point_value();
    CHECK_FALSE(empty);
    CHECK(empty.error == "invalid floating point: ''");

    auto invalid = args[3].duration_value();
    CHECK_FALSE(invalid);
    CHECK(invalid.error == "invalid duration: 'soon'");
}

TEST_CASE("Command line parser parses optional short inline values", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "define", .short_name = 'D', .value_mode = CommandLineValueMode::Optional},
        CommandLineOption{.long_name = "verbose", .short_name = 'v'},
    };
    char const* argv[] = {"program", "-Dname=value", "-D", "next", "-v", nullptr};

    CommandLineParser parser{5, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);

    CHECK(args[0].kind() == CommandLineArgumentKind::Option);
    CHECK(args[0].name() == "define");
    REQUIRE(args[0].value());
    CHECK(*args[0].value() == "name=value");
    CHECK(args[0].value_is_inline());

    CHECK(args[1].kind() == CommandLineArgumentKind::Option);
    CHECK(args[1].name() == "define");
    CHECK_FALSE(args[1].has_value());
    CHECK_FALSE(args[1].missing_value());

    CHECK(args[2].kind() == CommandLineArgumentKind::Positional);
    CHECK(args[2].token() == "next");

    CHECK(args[3].kind() == CommandLineArgumentKind::Option);
    CHECK(args[3].name() == "verbose");
}

TEST_CASE("Command line parser expands grouped short flags", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "all",    .short_name = 'a'},
        CommandLineOption{.long_name = "binary", .short_name = 'b'},
        CommandLineOption{.long_name = "color",  .short_name = 'c'},
    };
    char const* argv[] = {"program", "-abc", nullptr};

    CommandLineParser parser{2, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 3);
    CHECK(args[0].short_name() == 'a');
    CHECK(args[0].token() == "-abc");
    CHECK(args[1].short_name() == 'b');
    CHECK(args[1].token() == "-abc");
    CHECK(args[2].short_name() == 'c');
    CHECK(args[2].token() == "-abc");
}

TEST_CASE("Command line parser treats arguments after terminator as positional", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "verbose", .short_name = 'v'},
    };
    char const* argv[] = {"program", "-v", "--", "--not-an-option", "-x", nullptr};

    CommandLineParser parser{5, argv, options};

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 4);
    CHECK(args[0].kind() == CommandLineArgumentKind::Option);
    CHECK(args[1].kind() == CommandLineArgumentKind::Terminator);
    CHECK(args[2].kind() == CommandLineArgumentKind::Positional);
    CHECK(args[2].token() == "--not-an-option");
    CHECK(args[3].kind() == CommandLineArgumentKind::Positional);
    CHECK(args[3].token() == "-x");
}

TEST_CASE("Command line parser stores views into argv values", "[core][command-line]")
{
    constexpr std::array options{
        CommandLineOption{.long_name = "output", .short_name = 'o', .value_mode = CommandLineValueMode::Required},
    };
    char        program[]  = "program";
    char        option[]   = "--output=first.txt";
    char        position[] = "input.txt";
    char const* argv[]     = {program, option, position, nullptr};

    CommandLineParser parser{3, argv, options};

    option[9]   = 'F';
    position[0] = 'I';

    auto const& args = parser.arguments();
    REQUIRE(args.size() == 2);
    CHECK(args[0].token() == "--output=First.txt");
    REQUIRE(args[0].value());
    CHECK(*args[0].value() == "First.txt");
    CHECK(args[1].token() == "Input.txt");
}

// NOLINTEND(readability-magic-numbers)
