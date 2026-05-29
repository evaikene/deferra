#include "ini_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono> // IWYU pragma: keep for std::chrono_literals
#include <filesystem>
#include <fstream>
#include <string>

using namespace jb::core;
using namespace std::chrono_literals;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

auto write_ini(std::string const& content) -> std::filesystem::path
{
    auto          path = std::filesystem::temp_directory_path() / std::filesystem::path{"deferra-ini-file-test.ini"};
    std::ofstream file{path};
    file << content;
    return path;
}

auto test_dir(std::string_view name) -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "deferra-ini-file-test" / std::filesystem::path{name};
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

auto write_ini(std::filesystem::path const& path, std::string const& content) -> std::filesystem::path
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path};
    file << content;
    return path;
}

} // anonymous namespace

TEST_CASE("INI file parses keys, comments, quoted values and repeated keys", "[core][ini]")
{
    auto const path = write_ini(R"(
# comment
; another comment
queue.priority = default
queue.priority = high
plain = value
left = "  keep spaces"
right = 'keep spaces  '
empty =
)");

    IniFile ini{path};
    REQUIRE(ini.ok());
    CHECK(ini.contains("queue.priority"));
    CHECK_FALSE(ini.contains("missing"));
    CHECK(ini.size() == 5);

    auto const* priorities = ini.values("queue.priority");
    REQUIRE(priorities != nullptr);
    REQUIRE(priorities->size() == 2);
    CHECK((*priorities)[0] == "default");
    CHECK((*priorities)[1] == "high");
    CHECK(ini.value("queue.priority") == "high");
    CHECK(ini.value_or("missing", "fallback") == "fallback");
    CHECK(ini.value("left") == "  keep spaces");
    CHECK(ini.value("right") == "keep spaces  ");
    CHECK(ini.value("empty") == "");
}

TEST_CASE("INI file typed getters return parsed values and defaults", "[core][ini]")
{
    auto const path = write_ini(R"(
workers = 8
ratio = 0.75
enabled = yes
disabled = maybe
timeout = 5m
)");

    IniFile ini{path};
    REQUIRE(ini.ok());

    auto const workers = ini.integer("workers");
    REQUIRE(workers);
    CHECK(*workers.value == 8);

    auto const missing_workers = ini.integer_or("missing-workers", 2);
    REQUIRE(missing_workers);
    CHECK(*missing_workers.value == 2);

    auto const ratio = ini.floating_point("ratio");
    REQUIRE(ratio);
    CHECK(*ratio.value == 0.75);

    auto const enabled = ini.boolean("enabled");
    REQUIRE(enabled);
    CHECK(*enabled.value);

    auto const disabled = ini.boolean("disabled");
    REQUIRE(disabled);
    CHECK_FALSE(*disabled.value);

    auto const timeout = ini.interval("timeout");
    REQUIRE(timeout);
    CHECK(*timeout.value == 5min);

    auto const default_timeout = ini.interval_or("missing-timeout", 3s);
    REQUIRE(default_timeout);
    CHECK(*default_timeout.value == 3s);
}

TEST_CASE("INI file typed getters report conversion errors", "[core][ini]")
{
    auto const path = write_ini(R"(
workers = many
ratio =
timeout = 1h30m
)");

    IniFile ini{path};
    REQUIRE(ini.ok());

    auto const workers = ini.integer("workers");
    CHECK_FALSE(workers);
    CHECK_FALSE(workers.error.empty());

    auto const ratio = ini.floating_point("ratio");
    CHECK_FALSE(ratio);
    CHECK_FALSE(ratio.error.empty());

    auto const timeout = ini.interval("timeout");
    CHECK_FALSE(timeout);
    CHECK_FALSE(timeout.error.empty());

    auto const missing = ini.integer("missing");
    CHECK_FALSE(missing);
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("INI file includes another file at the include statement position", "[core][ini]")
{
    auto const dir = test_dir("direct-include");
    write_ini(dir / "extra.ini", R"(
order = included
extra = yes
)");
    auto const path = write_ini(dir / "main.ini", R"(
order = before
include = extra.ini
order = after
)");

    IniFile ini{path};
    REQUIRE(ini.ok());

    auto const* order = ini.values("order");
    REQUIRE(order != nullptr);
    REQUIRE(order->size() == 3);
    CHECK((*order)[0] == "before");
    CHECK((*order)[1] == "included");
    CHECK((*order)[2] == "after");
    CHECK(ini.value("extra") == "yes");
}

TEST_CASE("INI file resolves nested relative includes from the current file", "[core][ini]")
{
    auto const dir = test_dir("nested-relative-include");
    write_ini(dir / "conf" / "nested.ini", "nested = yes\n");
    write_ini(dir / "conf" / "extra.ini", "include = nested.ini\nextra = yes\n");
    auto const path = write_ini(dir / "main.ini", "include = conf/extra.ini\n");

    IniFile ini{path};
    REQUIRE(ini.ok());
    CHECK(ini.value("extra") == "yes");
    CHECK(ini.value("nested") == "yes");
}

TEST_CASE("INI file resolves relative includes from the symlink path directory", "[core][ini]")
{
    auto const dir        = test_dir("symlink-relative-include");
    auto const actual_dir = dir / "actual";
    auto const link_dir   = dir / "links";
    auto const actual     = write_ini(actual_dir / "main.ini", "include = linked-extra.ini\n");
    write_ini(link_dir / "linked-extra.ini", "extra = yes\n");

    auto const      link = link_dir / "main.ini";
    std::error_code error;
    std::filesystem::create_symlink(actual, link, error);
    if (error) {
        SUCCEED("filesystem does not permit symlink creation");
        return;
    }

    IniFile ini{link};
    REQUIRE(ini.ok());
    CHECK(ini.value("extra") == "yes");
}

TEST_CASE("INI file expands glob includes in alphabetical order", "[core][ini]")
{
    auto const dir = test_dir("glob-include");
    write_ini(dir / "conf.d" / "01-second.ini", "order = second\n");
    write_ini(dir / "conf.d" / "00-first.ini", "order = first\n");
    auto const path = write_ini(dir / "main.ini", "include = conf.d/*.ini\n");

    IniFile ini{path};
    REQUIRE(ini.ok());

    auto const* order = ini.values("order");
    REQUIRE(order != nullptr);
    REQUIRE(order->size() == 2);
    CHECK((*order)[0] == "first");
    CHECK((*order)[1] == "second");
}

TEST_CASE("INI file treats empty and unmatched glob includes as no-ops", "[core][ini]")
{
    auto const dir  = test_dir("empty-glob-include");
    auto const path = write_ini(dir / "main.ini", R"(
include =
include = conf.d/*.ini
present = yes
)");

    IniFile ini{path};
    REQUIRE(ini.ok());
    CHECK(ini.value("present") == "yes");
}

TEST_CASE("INI file reports missing specific include files", "[core][ini]")
{
    auto const dir  = test_dir("missing-specific-include");
    auto const path = write_ini(dir / "main.ini", "include = missing.ini\n");

    IniFile ini{path};
    CHECK_FALSE(ini.ok());
    CHECK_FALSE(ini.error().empty());
}

TEST_CASE("INI file rejects recursive includes", "[core][ini]")
{
    auto const dir = test_dir("recursive-include");
    write_ini(dir / "extra.ini", "include = main.ini\n");
    auto const path = write_ini(dir / "main.ini", "include = extra.ini\n");

    IniFile ini{path};
    CHECK_FALSE(ini.ok());
    CHECK_FALSE(ini.error().empty());
}

TEST_CASE("INI file reports the file path for parse errors in included files", "[core][ini]")
{
    auto const dir  = test_dir("included-parse-error");
    auto const path = write_ini(dir / "main.ini", "include = bad.ini\n");
    write_ini(dir / "bad.ini", "[unsupported-section]\n");

    IniFile ini{path};
    CHECK_FALSE(ini.ok());
    CHECK(ini.error().find("bad.ini") != std::string::npos);
    CHECK(ini.error().find("line 1") != std::string::npos);
}

TEST_CASE("INI file reports parse and open errors", "[core][ini]")
{
    auto const bad_path = write_ini("[unsupported-section]\n");
    IniFile    bad{bad_path};
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(bad.error().empty());

    IniFile missing{std::filesystem::temp_directory_path() / "deferra-missing.ini"};
    CHECK_FALSE(missing.ok());
    CHECK_FALSE(missing.error().empty());
}

// NOLINTEND(readability-magic-numbers)
