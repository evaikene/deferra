#include "cron_timezone_priv.hpp"

#include "cron.hpp"
#include "support/temporary_directory.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;

namespace {

struct FixtureType {
    std::int32_t utc_offset_seconds{};
    std::uint8_t daylight{};
    std::uint8_t abbreviation_index{};
};

struct FixtureSection {
    std::vector<std::int64_t>                          transitions;
    std::vector<std::uint8_t>                          transition_types;
    std::vector<FixtureType>                           local_time_types;
    std::string                                        abbreviations;
    std::vector<std::pair<std::int64_t, std::int32_t>> leap_seconds;
    std::vector<std::uint8_t>                          standard_wall_indicators;
    std::vector<std::uint8_t>                          utc_local_indicators;
};

struct TzifFixture {
    std::string bytes;
    std::size_t second_header_offset{};
    std::size_t second_section_offset{};
};

void append_u32(std::string& output, std::uint32_t value)
{
    for (auto shift : {24U, 16U, 8U, 0U}) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void append_i32(std::string& output, std::int32_t value)
{
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

void append_i64(std::string& output, std::int64_t value)
{
    auto const encoded = std::bit_cast<std::uint64_t>(value);
    for (auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U}) {
        output.push_back(static_cast<char>((encoded >> shift) & 0xffU));
    }
}

void replace_u32(std::string& output, std::size_t position, std::uint32_t value)
{
    REQUIRE(position + 4 <= output.size());
    for (auto shift : {24U, 16U, 8U, 0U}) {
        output[position++] = static_cast<char>((value >> shift) & 0xffU);
    }
}

void append_header(std::string& output, char version, FixtureSection const& section)
{
    REQUIRE(section.transitions.size() == section.transition_types.size());
    REQUIRE(section.local_time_types.size() <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(section.abbreviations.size() <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(section.transitions.size() <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(section.leap_seconds.size() <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(section.standard_wall_indicators.size() <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(section.utc_local_indicators.size() <= std::numeric_limits<std::uint32_t>::max());

    output.append("TZif", 4);
    output.push_back(version);
    output.append(15, '\0');
    append_u32(output, static_cast<std::uint32_t>(section.utc_local_indicators.size()));
    append_u32(output, static_cast<std::uint32_t>(section.standard_wall_indicators.size()));
    append_u32(output, static_cast<std::uint32_t>(section.leap_seconds.size()));
    append_u32(output, static_cast<std::uint32_t>(section.transitions.size()));
    append_u32(output, static_cast<std::uint32_t>(section.local_time_types.size()));
    append_u32(output, static_cast<std::uint32_t>(section.abbreviations.size()));
}

void append_section(std::string& output, FixtureSection const& section, std::size_t time_width)
{
    for (auto const transition : section.transitions) {
        if (time_width == 8) {
            append_i64(output, transition);
        }
        else {
            REQUIRE(transition >= std::numeric_limits<std::int32_t>::min());
            REQUIRE(transition <= std::numeric_limits<std::int32_t>::max());
            append_i32(output, static_cast<std::int32_t>(transition));
        }
    }
    for (auto const type : section.transition_types) {
        output.push_back(static_cast<char>(type));
    }
    for (auto const& type : section.local_time_types) {
        append_i32(output, type.utc_offset_seconds);
        output.push_back(static_cast<char>(type.daylight));
        output.push_back(static_cast<char>(type.abbreviation_index));
    }
    output.append(section.abbreviations);
    for (auto const& [transition, correction] : section.leap_seconds) {
        if (time_width == 8) {
            append_i64(output, transition);
        }
        else {
            REQUIRE(transition >= std::numeric_limits<std::int32_t>::min());
            REQUIRE(transition <= std::numeric_limits<std::int32_t>::max());
            append_i32(output, static_cast<std::int32_t>(transition));
        }
        append_i32(output, correction);
    }
    for (auto const indicator : section.standard_wall_indicators) {
        output.push_back(static_cast<char>(indicator));
    }
    for (auto const indicator : section.utc_local_indicators) {
        output.push_back(static_cast<char>(indicator));
    }
}

auto minimal_section(std::int64_t transition = 0, std::int32_t standard_offset = 0) -> FixtureSection
{
    return {
        .transitions      = {transition},
        .transition_types = {1},
        .local_time_types =
            {
                             {.utc_offset_seconds = standard_offset, .daylight = 0, .abbreviation_index = 0},
                             {.utc_offset_seconds = standard_offset + 3600, .daylight = 1, .abbreviation_index = 4},
                             },
        .abbreviations            = std::string{"STD\0DST\0", 8},
        .leap_seconds             = {},
        .standard_wall_indicators = {},
        .utc_local_indicators     = {},
    };
}

auto make_tzif(char                  version,
               FixtureSection const& first,
               FixtureSection const& second,
               std::string_view      footer = "STD0DST,M3.2.0/2,M11.1.0/2") -> TzifFixture
{
    TzifFixture fixture;
    append_header(fixture.bytes, version, first);
    append_section(fixture.bytes, first, 4);
    if (version == '\0') {
        return fixture;
    }

    fixture.second_header_offset = fixture.bytes.size();
    append_header(fixture.bytes, version, second);
    fixture.second_section_offset = fixture.bytes.size();
    append_section(fixture.bytes, second, 8);
    fixture.bytes.push_back('\n');
    fixture.bytes.append(footer);
    fixture.bytes.push_back('\n');
    return fixture;
}

auto make_tzif(char version, FixtureSection const& section, std::string_view footer = "STD0") -> TzifFixture
{
    return make_tzif(version, section, section, footer);
}

void check_invalid_data(std::string_view bytes)
{
    auto const parsed = parse_timezone_data(bytes);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().category == ErrorCategory::Internal);
    CHECK(parsed.error().code == "jobu.schedule.invalid_timezone_data");
}

void write_file(std::filesystem::path const& path, std::string_view bytes)
{
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

auto parsed_time(std::string_view text) -> UtcTimePoint
{
    auto parsed = parse_utc_timestamp(text);
    REQUIRE(parsed);
    return *parsed;
}

auto unix_seconds(std::string_view text) -> std::int64_t
{
    return std::chrono::floor<std::chrono::seconds>(parsed_time(text).time_since_epoch()).count();
}

auto local_time(std::string_view text) -> TimezoneLocalTimePoint
{
    return TimezoneLocalTimePoint{parsed_time(text).time_since_epoch()};
}

void check_local_mapping(TimezoneData const& data, std::string_view local, std::string_view expected_utc)
{
    auto mapped = timezone_utc_time(data, local_time(local));
    REQUIRE(mapped);
    CHECK(*mapped == parsed_time(expected_utc));
}

void check_utc_mapping(TimezoneData const& data, std::string_view utc, std::string_view expected_local)
{
    auto mapped = timezone_local_time(data, parsed_time(utc));
    REQUIRE(mapped);
    CHECK(*mapped == local_time(expected_local));
}

void check_next(SystemCronEngine const& engine,
                CronSchedule const&     schedule,
                std::string_view        lower_bound,
                std::string_view        expected)
{
    auto next = engine.next_after(schedule, parsed_time(lower_bound));
    if (!next) {
        FAIL("Required Linux timezone '" << schedule.timezone << "' failed: " << next.error().code << " ("
                                         << next.error().message << ')');
    }
    CHECK(*next == parsed_time(expected));
}

} // namespace

TEST_CASE("TZif versions 1 through 4 produce checked immutable transition data", "[jobu][cron][timezone]")
{
    auto first  = minimal_section(100);
    auto second = minimal_section(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 42);
    second.transition_types[0]                    = 0;
    second.local_time_types[0].utc_offset_seconds = 7200;

    auto const version_one = parse_timezone_data(make_tzif('\0', first).bytes);
    REQUIRE(version_one);
    REQUIRE(version_one->transitions.size() == 1);
    CHECK(version_one->transitions[0] == TimezoneTransition{.unix_seconds = 100, .local_time_type = 1});
    CHECK(version_one->local_time_types[1].daylight);
    CHECK(version_one->default_local_time_type == 0);
    CHECK_FALSE(version_one->future_rule);

    for (auto const version : {'2', '3', '4'}) {
        auto const parsed = parse_timezone_data(make_tzif(version, first, second).bytes);
        REQUIRE(parsed);
        REQUIRE(parsed->transitions.size() == 1);
        CHECK(parsed->transitions[0].unix_seconds ==
              static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 42);
        CHECK(parsed->transitions[0].local_time_type == 0);
        CHECK(parsed->local_time_types[0].utc_offset_seconds == 7200);
        REQUIRE(parsed->future_rule);
        CHECK(parsed->future_rule->standard_abbreviation == "STD");
        CHECK(parsed->future_rule->standard_utc_offset_seconds == 0);
        REQUIRE(parsed->future_rule->daylight);
        CHECK(parsed->future_rule->daylight->abbreviation == "DST");
        CHECK(parsed->future_rule->daylight->utc_offset_seconds == 3600);
    }
}

TEST_CASE("TZif POSIX footer accepts IANA rule forms and checked signed offsets", "[jobu][cron][timezone]")
{
    auto const section = minimal_section();

    auto const empty = parse_timezone_data(make_tzif('4', section, "").bytes);
    REQUIRE(empty);
    CHECK_FALSE(empty->future_rule);

    auto const fixed = parse_timezone_data(make_tzif('4', section, "<+03>-3").bytes);
    REQUIRE(fixed);
    REQUIRE(fixed->future_rule);
    CHECK(fixed->future_rule->standard_abbreviation == "+03");
    CHECK(fixed->future_rule->standard_utc_offset_seconds == 10800);
    CHECK_FALSE(fixed->future_rule->daylight);

    auto const explicit_offset = parse_timezone_data(make_tzif('4', section, "EST5EDT4,M3.2.0/-2:30:15,J300/26").bytes);
    REQUIRE(explicit_offset);
    REQUIRE(explicit_offset->future_rule);
    REQUIRE(explicit_offset->future_rule->daylight);
    auto const& explicit_daylight = *explicit_offset->future_rule->daylight;
    CHECK(explicit_daylight.utc_offset_seconds == -14400);
    CHECK(explicit_daylight.start.kind == PosixDateRuleKind::MonthWeekday);
    CHECK(explicit_daylight.start.month == 3);
    CHECK(explicit_daylight.start.week == 2);
    CHECK(explicit_daylight.start.weekday == 0);
    CHECK(explicit_daylight.start.transition_time_seconds == -9015);
    CHECK(explicit_daylight.end.kind == PosixDateRuleKind::JulianWithoutLeapDay);
    CHECK(explicit_daylight.end.day == 300);
    CHECK(explicit_daylight.end.transition_time_seconds == 93600);

    auto const default_offset = parse_timezone_data(make_tzif('4', section, "ABC0DEF,60,300").bytes);
    REQUIRE(default_offset);
    REQUIRE(default_offset->future_rule);
    REQUIRE(default_offset->future_rule->daylight);
    CHECK(default_offset->future_rule->daylight->utc_offset_seconds == 3600);
    CHECK(default_offset->future_rule->daylight->start.kind == PosixDateRuleKind::JulianWithLeapDay);
    CHECK(default_offset->future_rule->daylight->start.day == 60);
    CHECK(default_offset->future_rule->daylight->start.transition_time_seconds == 7200);

    auto no_transitions = section;
    no_transitions.transitions.clear();
    no_transitions.transition_types.clear();
    auto const all_future = parse_timezone_data(make_tzif('4', no_transitions, "ABC0").bytes);
    REQUIRE(all_future);
    CHECK(all_future->transitions.empty());
    REQUIRE(all_future->future_rule);

    REQUIRE(parse_timezone_data(make_tzif('4', section, "ABC24:59:59").bytes));
    REQUIRE(parse_timezone_data(make_tzif('4', section, "ABC-25:59:59").bytes));
    REQUIRE(parse_timezone_data(make_tzif('4', section, "ABC0DEF,M3.2.0/167:59:59,M11.1.0/-167").bytes));
}

TEST_CASE("TZif POSIX footer rejects unsupported or malformed grammar", "[jobu][cron][timezone]")
{
    auto const section = minimal_section();
    for (auto const* const footer : {
             "AB0",
             "<+3>-3",
             "<ABC0",
             "ABC",
             "ABC200",
             "ABC25",
             "ABC-26",
             "ABC0DEF",
             "ABC-25:59:59DEF,M3.2.0,M11.1.0",
             "ABC0DEF,M0.1.0,M11.1.0",
             "ABC0DEF,M3.0.0,M11.1.0",
             "ABC0DEF,M3.1.7,M11.1.0",
             "ABC0DEF,J0,J300",
             "ABC0DEF,J366,J300",
             "ABC0DEF,366,300",
             "ABC0DEF,M3.2.0/168,M11.1.0",
             "ABC0DEF,M3.2.0/2:60,M11.1.0",
             "ABC0DEF,M3.2.0,M11.1.0/trailing",
         }) {
        check_invalid_data(make_tzif('4', section, footer).bytes);
    }

    auto framed = make_tzif('4', section, "ABC0").bytes;
    framed.pop_back();
    check_invalid_data(framed);

    framed = make_tzif('4', section, "ABC0").bytes;
    framed.append("extra");
    check_invalid_data(framed);
}

TEST_CASE("TZif rejects malformed headers, counts, and every truncation", "[jobu][cron][timezone]")
{
    auto const section = minimal_section();
    auto       valid   = make_tzif('4', section).bytes;

    for (std::size_t size = 0; size < valid.size(); ++size) {
        check_invalid_data(std::string_view{valid}.substr(0, size));
    }

    auto malformed = valid;
    malformed[0]   = 'X';
    check_invalid_data(malformed);

    malformed    = valid;
    malformed[4] = '5';
    check_invalid_data(malformed);

    malformed    = valid;
    malformed[5] = 1;
    check_invalid_data(malformed);

    malformed = valid;
    replace_u32(malformed, 36, 0);
    check_invalid_data(malformed);

    malformed = valid;
    replace_u32(malformed, 32, 1000001);
    check_invalid_data(malformed);

    malformed = valid;
    replace_u32(malformed, 36, 257);
    check_invalid_data(malformed);

    malformed = valid;
    replace_u32(malformed, 40, 65537);
    check_invalid_data(malformed);

    malformed = valid;
    replace_u32(malformed, 24, 1);
    check_invalid_data(malformed);

    auto v1_with_trailing = make_tzif('\0', section).bytes;
    v1_with_trailing.push_back('\0');
    check_invalid_data(v1_with_trailing);

    check_invalid_data(std::string((16U * 1024U * 1024U) + 1U, '\0'));
}

TEST_CASE("TZif validates transitions, types, abbreviations, leaps, and indicators", "[jobu][cron][timezone]")
{
    auto section             = minimal_section();
    section.transitions      = {20, 10};
    section.transition_types = {0, 1};
    check_invalid_data(make_tzif('\0', section).bytes);

    section                     = minimal_section();
    section.transition_types[0] = 2;
    check_invalid_data(make_tzif('\0', section).bytes);

    section                                        = minimal_section();
    section.local_time_types[0].utc_offset_seconds = -90000;
    check_invalid_data(make_tzif('\0', section).bytes);

    section                                        = minimal_section();
    section.local_time_types[0].utc_offset_seconds = 93600;
    check_invalid_data(make_tzif('\0', section).bytes);

    section                              = minimal_section();
    section.local_time_types[0].daylight = 2;
    check_invalid_data(make_tzif('\0', section).bytes);

    section                                        = minimal_section();
    section.local_time_types[1].abbreviation_index = 8;
    check_invalid_data(make_tzif('\0', section).bytes);

    section                      = minimal_section();
    section.abbreviations.back() = 'X';
    check_invalid_data(make_tzif('\0', section).bytes);

    section              = minimal_section();
    section.leap_seconds = {
        {100, 1},
        {99,  2}
    };
    check_invalid_data(make_tzif('\0', section).bytes);

    section                          = minimal_section();
    section.standard_wall_indicators = {0, 2};
    section.utc_local_indicators     = {1, 0};
    check_invalid_data(make_tzif('\0', section).bytes);

    section                          = minimal_section();
    section.standard_wall_indicators = {0, 1};
    section.utc_local_indicators     = {1, 0};
    check_invalid_data(make_tzif('\0', section).bytes);

    section                          = minimal_section();
    section.standard_wall_indicators = {1, 1};
    section.utc_local_indicators     = {1, 0};
    section.leap_seconds             = {
        {100, 1},
        {200, 2}
    };
    REQUIRE(parse_timezone_data(make_tzif('4', section).bytes));

    section              = minimal_section();
    section.leap_seconds = {
        {100, 1},
        {200, 1}
    };
    REQUIRE(parse_timezone_data(make_tzif('4', section).bytes));
    check_invalid_data(make_tzif('3', section).bytes);

    section              = minimal_section();
    section.leap_seconds = {
        {100, 2}
    };
    REQUIRE(parse_timezone_data(make_tzif('4', section).bytes));
    check_invalid_data(make_tzif('3', section).bytes);

    section                  = minimal_section();
    section.local_time_types = {
        {.utc_offset_seconds = 3600, .daylight = 1, .abbreviation_index = 4},
        {.utc_offset_seconds = 0,    .daylight = 0, .abbreviation_index = 0},
    };
    section.transition_types       = {0};
    auto const before_first_result = parse_timezone_data(make_tzif('\0', section).bytes);
    REQUIRE(before_first_result);
    CHECK(before_first_result->default_local_time_type == 0);
}

TEST_CASE("TZif validates compatibility and preferred sections independently", "[jobu][cron][timezone]")
{
    auto first   = minimal_section(10);
    auto second  = minimal_section(20);
    auto fixture = make_tzif('4', first, second);

    auto       malformed_first   = fixture.bytes;
    auto const first_type_offset = 44U + (first.transitions.size() * 4U) + first.transition_types.size();
    replace_u32(malformed_first, first_type_offset, std::bit_cast<std::uint32_t>(std::int32_t{-90000}));
    check_invalid_data(malformed_first);

    auto       malformed_second = fixture.bytes;
    auto const second_type_offset =
        fixture.second_section_offset + (second.transitions.size() * 8U) + second.transition_types.size();
    replace_u32(malformed_second, second_type_offset, std::bit_cast<std::uint32_t>(std::int32_t{93600}));
    check_invalid_data(malformed_second);

    auto mismatched_header                              = fixture.bytes;
    mismatched_header[fixture.second_header_offset + 4] = '3';
    check_invalid_data(mismatched_header);
}

TEST_CASE("Timezone lookup validates names and keeps canonical targets inside their root", "[jobu][cron][timezone]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   root   = directory.path() / "zoneinfo";
    auto const                   region = root / "Region";
    std::error_code              error;
    REQUIRE(std::filesystem::create_directories(region, error));
    REQUIRE_FALSE(error);

    auto const fixture = make_tzif('4', minimal_section(0, 7200), "<+02>-2").bytes;
    write_file(region / "Test", fixture);
    auto const roots = std::array{root};

    auto loaded = load_timezone_data("Region/Test", roots);
    REQUIRE(loaded);
    CHECK(loaded->local_time_types[0].utc_offset_seconds == 7200);

    write_file(region /
                   std::filesystem::path{
                       std::string{"\xc3\x9c", 2}
    },
               fixture);
    REQUIRE(load_timezone_data(std::string{"Region/\xc3\x9c", 9}, roots));

    for (auto const& name : {
             std::string{},
             std::string{"/Region/Test"},
             std::string{"Region//Test"},
             std::string{"Region/Test/"},
             std::string{"Region/./Test"},
             std::string{"Region/../Test"},
             std::string{"Region\\Test"},
             std::string{"Region/\nTest"},
             std::string{"Region/\rTest"},
             std::string{"Region/\tTest"},
             std::string{"Region/\x7f", 8},
             std::string(256, 'x'),
             std::string{"\xc0\x80", 2},
    }) {
        auto const invalid = load_timezone_data(name, roots);
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error().category == ErrorCategory::InvalidArgument);
        CHECK(invalid.error().code == "jobu.schedule.invalid_timezone");
    }

    auto embedded_nul = std::string{"Region/Test"};
    embedded_nul[6]   = '\0';
    CHECK_FALSE(load_timezone_data(embedded_nul, roots));
    CHECK_FALSE(load_timezone_data("Region/Missing", roots));

    std::filesystem::create_symlink(region / "Test", root / "Alias", error);
    REQUIRE_FALSE(error);
    REQUIRE(load_timezone_data("Alias", roots));

    auto const outside = directory.path() / "outside.tzif";
    write_file(outside, fixture);
    std::filesystem::create_symlink(outside, root / "Escape", error);
    REQUIRE_FALSE(error);
    auto const escaped = load_timezone_data("Escape", roots);
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == "jobu.schedule.invalid_timezone");

    auto const prefix_sibling = directory.path() / "zoneinfo-old";
    REQUIRE(std::filesystem::create_directory(prefix_sibling, error));
    REQUIRE_FALSE(error);
    write_file(prefix_sibling / "Zone", fixture);
    std::filesystem::create_symlink(prefix_sibling / "Zone", root / "PrefixEscape", error);
    REQUIRE_FALSE(error);
    CHECK_FALSE(load_timezone_data("PrefixEscape", roots));

    REQUIRE(std::filesystem::create_directory(root / "Directory", error));
    REQUIRE_FALSE(error);
    CHECK_FALSE(load_timezone_data("Directory", roots));
}

TEST_CASE("Timezone lookup uses the first valid root and bounds files before reading", "[jobu][cron][timezone]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   first_root  = directory.path() / "first";
    auto const                   second_root = directory.path() / "second";
    std::error_code              error;
    REQUIRE(std::filesystem::create_directories(first_root / "Region", error));
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::create_directories(second_root / "Region", error));
    REQUIRE_FALSE(error);

    write_file(first_root / "Region" / "Test", make_tzif('4', minimal_section(0, 3600), "ABC-1").bytes);
    write_file(second_root / "Region" / "Test", make_tzif('4', minimal_section(0, 7200), "ABC-2").bytes);

    auto const roots  = std::array{first_root, second_root};
    auto const loaded = load_timezone_data("Region/Test", roots);
    REQUIRE(loaded);
    CHECK(loaded->local_time_types[0].utc_offset_seconds == 3600);

    write_file(first_root / "Invalid", "not a timezone");
    auto const malformed = load_timezone_data("Invalid", roots);
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().category == ErrorCategory::Internal);
    CHECK(malformed.error().code == "jobu.schedule.invalid_timezone_data");

    write_file(first_root / "TooLarge", std::string((16U * 1024U * 1024U) + 1U, '\0'));
    auto const oversized = load_timezone_data("TooLarge", roots);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobu.schedule.invalid_timezone_data");
}

TEST_CASE("Timezone mapping uses explicit intervals, the pre-transition type, and overlap policy",
          "[jobu][cron][timezone]")
{
    auto const data = TimezoneData{
        .transitions =
            {
                          {.unix_seconds = unix_seconds("2024-03-31T01:00:00Z"), .local_time_type = 1},
                          {.unix_seconds = unix_seconds("2024-10-27T01:00:00Z"), .local_time_type = 0},
                          },
        .local_time_types =
            {
                          {.utc_offset_seconds = 0, .daylight = false},
                          {.utc_offset_seconds = 3600, .daylight = true},
                          },
        .default_local_time_type = 0,
        .future_rule             = std::nullopt,
    };

    check_utc_mapping(data, "2024-01-15T12:00:00Z", "2024-01-15T12:00:00Z");
    check_utc_mapping(data, "2024-06-15T12:00:00Z", "2024-06-15T13:00:00Z");
    check_local_mapping(data, "2024-06-15T13:00:00Z", "2024-06-15T12:00:00Z");

    check_local_mapping(data, "2024-03-31T01:30:00Z", "2024-03-31T01:30:00Z");
    check_local_mapping(data, "2024-10-27T01:30:00Z", "2024-10-27T00:30:00Z");
}

TEST_CASE("Timezone gaps shift by their exact non-hour offset change", "[jobu][cron][timezone]")
{
    auto const data = TimezoneData{
        .transitions =
            {
                          {.unix_seconds = unix_seconds("2024-03-31T01:00:00Z"), .local_time_type = 1},
                          {.unix_seconds = unix_seconds("2024-10-27T10:00:00Z"), .local_time_type = 0},
                          },
        .local_time_types =
            {
                          {.utc_offset_seconds = 0, .daylight = false},
                          {.utc_offset_seconds = 1800, .daylight = true},
                          },
        .default_local_time_type = 0,
        .future_rule             = std::nullopt,
    };

    check_local_mapping(data, "2024-03-31T01:15:00Z", "2024-03-31T01:15:00Z");
    check_local_mapping(data, "2024-10-27T10:15:00Z", "2024-10-27T09:45:00Z");
}

TEST_CASE("POSIX future rules cover leap-sensitive dates and out-of-day transitions", "[jobu][cron][timezone]")
{
    auto leap_rule = [](PosixDateRuleKind kind, std::uint16_t day) {
        return TimezoneData{
            .transitions             = {},
            .local_time_types        = {{.utc_offset_seconds = 0, .daylight = false}},
            .default_local_time_type = 0,
            .future_rule =
                PosixFutureRule{
                                        .standard_abbreviation       = "STD",
                                        .standard_utc_offset_seconds = 0,
                                        .daylight =
                        PosixDaylightRule{
                            .abbreviation       = "DST",
                            .utc_offset_seconds = 3600,
                            .start =
                                PosixDateRule{
                                    .kind                    = kind,
                                    .day                     = day,
                                    .transition_time_seconds = 0,
                                },
                            .end =
                                PosixDateRule{
                                    .kind                    = kind,
                                    .day                     = static_cast<std::uint16_t>(day + 1),
                                    .transition_time_seconds = 0,
                                },
                        }, },
        };
    };

    auto const julian = leap_rule(PosixDateRuleKind::JulianWithoutLeapDay, 60);
    check_local_mapping(julian, "2024-03-01T00:30:00Z", "2024-03-01T00:30:00Z");

    auto const numeric = leap_rule(PosixDateRuleKind::JulianWithLeapDay, 59);
    check_local_mapping(numeric, "2024-02-29T00:30:00Z", "2024-02-29T00:30:00Z");
    check_local_mapping(numeric, "2023-03-01T00:30:00Z", "2023-03-01T00:30:00Z");

    auto const outside_day = TimezoneData{
        .transitions             = {},
        .local_time_types        = {{.utc_offset_seconds = 0, .daylight = false}},
        .default_local_time_type = 0,
        .future_rule =
            PosixFutureRule{
                                    .standard_abbreviation       = "STD",
                                    .standard_utc_offset_seconds = 0,
                                    .daylight =
                    PosixDaylightRule{
                        .abbreviation       = "DST",
                        .utc_offset_seconds = 3600,
                        .start =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 3,
                                .week                    = 2,
                                .weekday                 = 0,
                                .transition_time_seconds = -3600,
                            },
                        .end =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 11,
                                .week                    = 1,
                                .weekday                 = 0,
                                .transition_time_seconds = 26 * 3600,
                            },
                    }, },
    };
    check_local_mapping(outside_day, "2024-03-09T23:30:00Z", "2024-03-09T23:30:00Z");
    check_local_mapping(outside_day, "2024-11-04T01:30:00Z", "2024-11-04T00:30:00Z");
}

TEST_CASE("POSIX future rules take over only after the last explicit transition", "[jobu][cron][timezone]")
{
    auto const data = TimezoneData{
        .transitions =
            {
                          {.unix_seconds = unix_seconds("2037-11-01T06:00:00Z"), .local_time_type = 0},
                          },
        .local_time_types =
            {
                          {.utc_offset_seconds = -18000, .daylight = false},
                          {.utc_offset_seconds = -14400, .daylight = true},
                          },
        .default_local_time_type = 0,
        .future_rule =
            PosixFutureRule{
                          .standard_abbreviation       = "EST",
                          .standard_utc_offset_seconds = -18000,
                          .daylight =
                    PosixDaylightRule{
                        .abbreviation       = "EDT",
                        .utc_offset_seconds = -14400,
                        .start =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 3,
                                .week                    = 2,
                                .weekday                 = 0,
                                .transition_time_seconds = 7200,
                            },
                        .end =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 11,
                                .week                    = 1,
                                .weekday                 = 0,
                                .transition_time_seconds = 7200,
                            },
                    }, },
    };

    check_utc_mapping(data, "2030-07-01T12:00:00Z", "2030-07-01T07:00:00Z");
    check_local_mapping(data, "2100-07-01T12:00:00Z", "2100-07-01T16:00:00Z");
}

TEST_CASE("POSIX future rules select the active season across a year boundary", "[jobu][cron][timezone]")
{
    auto const data = TimezoneData{
        .transitions             = {},
        .local_time_types        = {{.utc_offset_seconds = 37800, .daylight = false}},
        .default_local_time_type = 0,
        .future_rule =
            PosixFutureRule{
                                    .standard_abbreviation       = "LHST",
                                    .standard_utc_offset_seconds = 37800,
                                    .daylight =
                    PosixDaylightRule{
                        .abbreviation       = "LHDT",
                        .utc_offset_seconds = 39600,
                        .start =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 10,
                                .week                    = 1,
                                .weekday                 = 0,
                                .transition_time_seconds = 7200,
                            },
                        .end =
                            PosixDateRule{
                                .kind                    = PosixDateRuleKind::MonthWeekday,
                                .month                   = 4,
                                .week                    = 1,
                                .weekday                 = 0,
                                .transition_time_seconds = 7200,
                            },
                    }, },
    };

    check_local_mapping(data, "2024-01-15T12:00:00Z", "2024-01-15T01:00:00Z");
    check_local_mapping(data, "2024-07-15T12:00:00Z", "2024-07-15T01:30:00Z");
}

TEST_CASE("Timezone mapping reports malformed data and representable-range exhaustion", "[jobu][cron][timezone]")
{
    auto invalid = TimezoneData{};
    auto mapped  = timezone_utc_time(invalid, TimezoneLocalTimePoint{});
    REQUIRE_FALSE(mapped);
    CHECK(mapped.error().code == "jobu.schedule.invalid_timezone_data");

    auto const western = TimezoneData{
        .transitions             = {},
        .local_time_types        = {{.utc_offset_seconds = -3600, .daylight = false}},
        .default_local_time_type = 0,
        .future_rule             = std::nullopt,
    };
    mapped = timezone_utc_time(western, TimezoneLocalTimePoint{UtcTimePoint::max().time_since_epoch()});
    REQUIRE_FALSE(mapped);
    CHECK(mapped.error().category == ErrorCategory::ResourceExhausted);
    CHECK(mapped.error().code == "jobu.schedule.out_of_range");
}

TEST_CASE("System cron engine validates schedules and handles strict UTC occurrences", "[jobu][cron][system]")
{
    SystemCronEngine engine;
    REQUIRE(engine.validate({.expression = "*/15 * * * *", .timezone = "UTC"}));

    auto invalid_expression = engine.validate({.expression = "* * * *", .timezone = "UTC"});
    REQUIRE_FALSE(invalid_expression);
    CHECK(invalid_expression.error().code == "jobu.schedule.invalid_expression");

    auto invalid_timezone = engine.validate({.expression = "* * * * *", .timezone = "../UTC"});
    REQUIRE_FALSE(invalid_timezone);
    CHECK(invalid_timezone.error().code == "jobu.schedule.invalid_timezone");

    auto const every_minute = CronSchedule{.expression = "* * * * *", .timezone = "UTC"};
    check_next(engine, every_minute, "2024-05-17T12:34:00Z", "2024-05-17T12:35:00Z");
    check_next(engine, every_minute, "2024-05-17T12:34:00.000001Z", "2024-05-17T12:35:00Z");

    auto occurrences = next_cron_occurrences(engine,
                                             {.expression = "0 * * * *", .timezone = "UTC"},
                                             parsed_time("2024-05-17T12:00:00Z"),
                                             3);
    REQUIRE(occurrences);
    CHECK(*occurrences == std::vector<UtcTimePoint>{
                              parsed_time("2024-05-17T13:00:00Z"),
                              parsed_time("2024-05-17T14:00:00Z"),
                              parsed_time("2024-05-17T15:00:00Z"),
                          });

    for (auto const count : {0U, 201U}) {
        auto invalid_count = next_cron_occurrences(engine, every_minute, UtcTimePoint{}, count);
        REQUIRE_FALSE(invalid_count);
        CHECK(invalid_count.error().category == ErrorCategory::InvalidArgument);
        CHECK(invalid_count.error().code == "jobu.schedule.invalid_count");
    }

    auto exhausted =
        engine.next_after({.expression = "0 0 31 FEB *", .timezone = "UTC"}, parsed_time("2024-01-01T00:00:00Z"));
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().code == "jobu.schedule.no_future_occurrence");

    auto out_of_range = engine.next_after(every_minute, UtcTimePoint::max());
    REQUIRE_FALSE(out_of_range);
    CHECK(out_of_range.error().code == "jobu.schedule.out_of_range");
}

TEST_CASE("System cron engine applies installed Linux timezone gap and overlap rules", "[jobu][cron][system]")
{
    SystemCronEngine engine;

    check_next(engine,
               {.expression = "30 3 31 MAR *", .timezone = "Europe/Tallinn"},
               "2024-03-30T00:00:00Z",
               "2024-03-31T01:30:00Z");
    check_next(engine,
               {.expression = "30 3 27 OCT *", .timezone = "Europe/Tallinn"},
               "2024-10-26T00:00:00Z",
               "2024-10-27T00:30:00Z");

    check_next(engine,
               {.expression = "30 2 10 MAR *", .timezone = "America/New_York"},
               "2024-03-09T00:00:00Z",
               "2024-03-10T07:30:00Z");
    check_next(engine,
               {.expression = "30 1 3 NOV *", .timezone = "America/New_York"},
               "2024-11-02T00:00:00Z",
               "2024-11-03T05:30:00Z");
    check_next(engine,
               {.expression = "30 1 * * *", .timezone = "America/New_York"},
               "2024-11-03T06:15:00Z",
               "2024-11-04T06:30:00Z");

    check_next(engine,
               {.expression = "15 2 6 OCT *", .timezone = "Australia/Lord_Howe"},
               "2024-10-05T00:00:00Z",
               "2024-10-05T15:45:00Z");
    check_next(engine,
               {.expression = "45 1 7 APR *", .timezone = "Australia/Lord_Howe"},
               "2024-04-06T00:00:00Z",
               "2024-04-06T14:45:00Z");
}
