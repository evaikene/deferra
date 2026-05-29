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
