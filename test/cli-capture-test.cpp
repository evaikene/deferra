#include "cli_capture_priv.hpp"

#include "byte_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

using namespace jb::core;
using namespace jb::jobu::detail;

namespace {

void append(CliCaptureBuffer& capture, std::string_view bytes)
{
    REQUIRE(capture.append(as_bytes(bytes)));
}

void check_capture(std::size_t limit, std::string_view input, std::string_view expected)
{
    auto capture = CliCaptureBuffer{limit};
    append(capture, input);

    auto result = capture.take();
    CHECK(as_string_view(result.bytes) == expected);
    CHECK(result.total_bytes == input.size());
    CHECK(result.truncated == (input.size() > expected.size()));
}

} // anonymous namespace

TEST_CASE("CLI capture retains exact streams through its limit", "[jobu][cli][capture]")
{
    check_capture(0, "", "");
    check_capture(1, "a", "a");
    check_capture(2, "ab", "ab");
    check_capture(3, "abc", "abc");
    check_capture(4, "abcd", "abcd");
    check_capture(5, "abcde", "abcde");
}

TEST_CASE("CLI capture retains exact first and last bytes after truncation", "[jobu][cli][capture]")
{
    check_capture(0, "abc", "");
    check_capture(1, "ab", "a");
    check_capture(2, "abc", "ac");
    check_capture(3, "abcd", "abd");
    check_capture(4, "abcde", "abde");
    check_capture(5, "abcdef", "abcef");
    check_capture(6, "abcdefghijk", "abcijk");
}

TEST_CASE("CLI capture preserves suffix order across arbitrary chunks", "[jobu][cli][capture]")
{
    auto capture = CliCaptureBuffer{6};
    append(capture, "ab");
    append(capture, "cdef");
    append(capture, "g");
    append(capture, "hi");
    append(capture, "j");
    append(capture, "k");

    auto result = capture.take();
    CHECK(as_string_view(result.bytes) == "abcijk");
    CHECK(result.total_bytes == 11U);
    CHECK(result.truncated);
}

TEST_CASE("CLI capture checks total byte overflow", "[jobu][cli][capture]")
{
    auto const maximum = std::numeric_limits<std::uint64_t>::max();

    auto exact = checked_cli_capture_total(maximum - 6U, 6U);
    REQUIRE(exact);
    CHECK(*exact == maximum);

    auto overflow = checked_cli_capture_total(maximum, 1U);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().category == ErrorCategory::Internal);
    CHECK(overflow.error().code == "jobu.cli.capture_failed");
    CHECK(overflow.error().message == "CLI output capture failed");
    CHECK(overflow.error().detail == "capture_total_overflow");
}

TEST_CASE("CLI capture take resets retained and counted state", "[jobu][cli][capture]")
{
    auto capture = CliCaptureBuffer{3};
    append(capture, "abcdef");

    auto first = capture.take();
    REQUIRE(first.total_bytes == 6U);
    CHECK(first.truncated);

    append(capture, "xy");
    auto second = capture.take();
    CHECK(as_string_view(second.bytes) == "xy");
    CHECK(second.total_bytes == 2U);
    CHECK_FALSE(second.truncated);
}
