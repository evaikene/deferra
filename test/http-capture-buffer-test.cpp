#include "http_capture_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

using namespace jb::core;
using namespace jb::net;
using namespace jb::net::detail;

namespace {

void append(CaptureBuffer& capture, std::string_view bytes)
{
    auto result = capture.append(bytes.data(), 1U, bytes.size());
    REQUIRE(result);
    CHECK(*result == bytes.size());
}

void check_capture(std::size_t limit, std::string_view input, std::string_view expected)
{
    auto capture = CaptureBuffer{limit};
    append(capture, input);

    auto result = capture.take();
    CHECK(as_string_view(result.bytes) == expected);
    CHECK(result.total_bytes == input.size());
    CHECK(result.truncated == (input.size() > expected.size()));
}

void check_overflow(auto const& result, std::string_view detail)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Internal);
    CHECK(result.error().code == "net.http.internal");
    CHECK(result.error().message == "HTTP capture failed");
    CHECK(result.error().detail == detail);
}

} // anonymous namespace

TEST_CASE("HTTP capture retains exact streams through its limit", "[net][http][capture]")
{
    check_capture(0, "", "");
    check_capture(1, "a", "a");
    check_capture(2, "ab", "ab");
    check_capture(3, "abc", "abc");
    check_capture(4, "abcd", "abcd");
    check_capture(5, "abcde", "abcde");
}

TEST_CASE("HTTP capture retains first and last bytes after truncation", "[net][http][capture]")
{
    check_capture(0, "abc", "");
    check_capture(1, "ab", "a");
    check_capture(2, "abc", "ac");
    check_capture(3, "abcd", "abd");
    check_capture(4, "abcde", "abde");
    check_capture(5, "abcdef", "abcef");
    check_capture(6, "abcdefghijk", "abcijk");
}

TEST_CASE("HTTP capture preserves first and last ordering across callback chunks", "[net][http][capture]")
{
    auto capture = CaptureBuffer{6};
    append(capture, "ab");
    append(capture, "cdef");
    append(capture, "g");
    append(capture, "h");
    append(capture, "i");
    append(capture, "j");
    append(capture, "k");

    auto result = capture.take();
    CHECK(as_string_view(result.bytes) == "abcijk");
    CHECK(result.total_bytes == 11U);
    CHECK(result.truncated);
}

TEST_CASE("HTTP capture checks callback multiplication and cumulative count", "[net][http][capture]")
{
    auto bytes    = std::string_view{"abcdef"};
    auto capture  = CaptureBuffer{6};
    auto appended = capture.append(bytes.data(), 2U, 3U);
    REQUIRE(appended);
    CHECK(*appended == 6U);
    CHECK(as_string_view(capture.take().bytes) == bytes);

    auto exact = checked_capture_append_size(std::numeric_limits<std::uint64_t>::max() - 6U, 2U, 3U);
    REQUIRE(exact);
    CHECK(*exact == 6U);

    check_overflow(checked_capture_append_size(0, std::numeric_limits<std::size_t>::max(), 2U),
                   "capture_size_overflow");
    check_overflow(checked_capture_append_size(std::numeric_limits<std::uint64_t>::max(), 1U, 1U),
                   "capture_total_overflow");

    auto zero = checked_capture_append_size(std::numeric_limits<std::uint64_t>::max(),
                                            0U,
                                            std::numeric_limits<std::size_t>::max());
    REQUIRE(zero);
    CHECK(*zero == 0U);

    auto empty_capture = CaptureBuffer{1};
    auto empty_append  = empty_capture.append(nullptr, 0U, std::numeric_limits<std::size_t>::max());
    REQUIRE(empty_append);
    CHECK(*empty_append == 0U);
}

TEST_CASE("HTTP capture failures leave prior state unchanged", "[net][http][capture]")
{
    auto capture = CaptureBuffer{4};
    append(capture, "abc");

    check_overflow(capture.append(nullptr, 1U, 1U), "capture_null_data");
    auto result = capture.take();
    CHECK(as_string_view(result.bytes) == "abc");
    CHECK(result.total_bytes == 3U);
    CHECK_FALSE(result.truncated);
}

TEST_CASE("HTTP capture take resets retained and counted state", "[net][http][capture]")
{
    auto capture = CaptureBuffer{3};
    append(capture, "abcdef");
    auto first = capture.take();
    REQUIRE(first.total_bytes == 6U);

    append(capture, "xy");
    auto second = capture.take();
    CHECK(as_string_view(second.bytes) == "xy");
    CHECK(second.total_bytes == 2U);
    CHECK_FALSE(second.truncated);
}
