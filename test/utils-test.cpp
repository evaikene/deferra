#include "utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono> // IWYU pragma: keep for std::chrono_literals
#include <filesystem>
#include <fstream>

// NOLINTBEGIN(readability-magic-numbers)

using namespace jb::core;
using namespace std::chrono_literals;

namespace {

auto test_dir(std::string_view name) -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "deferra-utils-test" / std::filesystem::path{name};
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

auto write_file(std::filesystem::path const& path) -> std::filesystem::path
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path};
    file << "value\n";
    return path;
}

} // anonymous namespace

TEST_CASE("trim_ascii_whitespace removes leading and trailing ASCII whitespace", "[core][utils]")
{
    CHECK(trim_ascii_whitespace("  value\t\r\n") == "value");
    CHECK(trim_ascii_whitespace("value") == "value");
    CHECK(trim_ascii_whitespace(" \t\r\n") == "");
}

TEST_CASE("parse_integer requires a full integer string", "[core][utils]")
{
    auto const value = parse_integer(" -42 ");
    REQUIRE(value);
    CHECK(*value.value == -42);

    CHECK_FALSE(parse_integer("42x"));
    CHECK_FALSE(parse_integer(""));
}

TEST_CASE("parse_floating_point requires a full finite number string", "[core][utils]")
{
    auto const value = parse_floating_point(" 1.25 ");
    REQUIRE(value);
    CHECK(*value.value == 1.25);

    CHECK_FALSE(parse_floating_point("1,25"));
    CHECK_FALSE(parse_floating_point("1.25x"));
    CHECK_FALSE(parse_floating_point("nan"));
    CHECK_FALSE(parse_floating_point(""));
}

TEST_CASE("parse_boolean treats only enabled words as true", "[core][utils]")
{
    CHECK(parse_boolean("y"));
    CHECK(parse_boolean("YES"));
    CHECK(parse_boolean(" true "));
    CHECK(parse_boolean("on"));
    CHECK(parse_boolean("1"));

    CHECK_FALSE(parse_boolean("n"));
    CHECK_FALSE(parse_boolean("false"));
    CHECK_FALSE(parse_boolean("0"));
    CHECK_FALSE(parse_boolean("anything else"));
}

TEST_CASE("parse_duration supports single unit interval suffixes", "[core][utils]")
{
    auto const seconds = parse_duration("5s");
    REQUIRE(seconds);
    CHECK(*seconds.value == 5s);

    auto const minutes = parse_duration("2m");
    REQUIRE(minutes);
    CHECK(*minutes.value == 2min);

    auto const hours = parse_duration("3h");
    REQUIRE(hours);
    CHECK(*hours.value == 3h);

    auto const days = parse_duration("4d");
    REQUIRE(days);
    CHECK(*days.value == 96h);

    CHECK_FALSE(parse_duration("1h30m"));
    CHECK_FALSE(parse_duration("-1s"));
    CHECK_FALSE(parse_duration("10"));
}

TEST_CASE("has_glob_pattern detects wildcard path characters", "[core][utils]")
{
    CHECK(has_glob_pattern("conf.d/*.ini"));
    CHECK(has_glob_pattern("conf.d/[01]-file.ini"));
    CHECK(has_glob_pattern("conf.d/file?.ini"));

    CHECK_FALSE(has_glob_pattern("conf.d/file.ini"));
    CHECK_FALSE(has_glob_pattern(""));
}

TEST_CASE("expand_glob_paths returns sorted matches", "[core][utils]")
{
    auto const dir    = test_dir("glob-sorted");
    auto const first  = write_file(dir / "00-first.ini");
    auto const second = write_file(dir / "01-second.ini");
    write_file(dir / "ignored.txt");

    auto const paths = expand_glob_paths(dir / "*.ini");
    REQUIRE(paths);
    REQUIRE(paths.value->size() == 2);
    CHECK((*paths.value)[0] == first);
    CHECK((*paths.value)[1] == second);
}

TEST_CASE("expand_glob_paths returns an empty vector for unmatched patterns", "[core][utils]")
{
    auto const dir = test_dir("glob-empty");

    auto const paths = expand_glob_paths(dir / "*.ini");
    REQUIRE(paths);
    CHECK(paths.value->empty());
}

// NOLINTEND(readability-magic-numbers)
