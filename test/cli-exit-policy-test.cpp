#include "cli_exit_policy_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

auto selector_list(std::initializer_list<std::string_view> selectors) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(selectors.size());
    for (auto const selector : selectors) {
        result.push_back({.data = std::string{selector}});
    }
    return result;
}

} // anonymous namespace

TEST_CASE("CLI retry exit selectors normalize into one membership policy", "[jobu][cli][exit-policy]")
{
    auto decoded = decode_cli_retry_exit_codes(selector_list({"255", "7-9", "0", "2-4", "6", "5"}));
    REQUIRE(decoded);

    auto const& ranges = decoded->ranges();
    REQUIRE(ranges.size() == 3U);
    CHECK(ranges[0] == CliExitCodeRange{.first = 0, .last = 0});
    CHECK(ranges[1] == CliExitCodeRange{.first = 2, .last = 9});
    CHECK(ranges[2] == CliExitCodeRange{.first = 255, .last = 255});

    CHECK(decoded->contains(0));
    CHECK_FALSE(decoded->contains(1));
    CHECK(decoded->contains(2));
    CHECK(decoded->contains(9));
    CHECK_FALSE(decoded->contains(10));
    CHECK(decoded->contains(255));
    CHECK_FALSE(decoded->contains(-1));
    CHECK_FALSE(decoded->contains(256));
}

TEST_CASE("CLI retry exit selectors accept an empty terminal policy", "[jobu][cli][exit-policy]")
{
    auto decoded = decode_cli_retry_exit_codes({});
    REQUIRE(decoded);
    CHECK(decoded->ranges().empty());
    CHECK_FALSE(decoded->contains(0));
    CHECK_FALSE(decoded->contains(255));
}

TEST_CASE("CLI retry exit selectors require canonical bounded syntax", "[jobu][cli][exit-policy]")
{
    for (auto const* const selector : {
             "",
             "00",
             "01",
             "+1",
             "-1",
             " 1",
             "1 ",
             "256",
             "1-",
             "-",
             "1--2",
             "1-1",
             "2-1",
             "01-2",
             "1-02",
             "1-256",
         }) {
        CAPTURE(selector);
        CHECK_FALSE(decode_cli_retry_exit_codes(selector_list({selector})));
    }
}

TEST_CASE("CLI retry exit selectors reject duplicate and overlapping membership", "[jobu][cli][exit-policy]")
{
    for (auto const& selectors : {
             selector_list({"1", "1"}),
             selector_list({"1", "1-2"}),
             selector_list({"1-3", "3-5"}),
             selector_list({"10-20", "12"}),
             selector_list({"200-220", "190-210"}),
         }) {
        CHECK_FALSE(decode_cli_retry_exit_codes(selectors));
    }

    CHECK_FALSE(decode_cli_retry_exit_codes({AttributeValue{.data = std::int64_t{1}}}));
}

TEST_CASE("CLI retry exit selectors retain the input count bound before normalization", "[jobu][cli][exit-policy]")
{
    auto selectors = AttributeValue::List{};
    selectors.reserve(65U);
    for (std::uint16_t code = 0; code < 128; code += 2) {
        selectors.push_back({.data = std::to_string(code)});
    }

    auto maximum = decode_cli_retry_exit_codes(selectors);
    REQUIRE(maximum);
    CHECK(maximum->ranges().size() == 64U);

    selectors.push_back({.data = std::string{"128"}});
    CHECK_FALSE(decode_cli_retry_exit_codes(selectors));
}
