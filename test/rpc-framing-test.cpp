#include "framing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::rpc;

namespace {

auto encoded(std::string_view body, FramingLimits limits = {}) -> std::string
{
    auto result = frame_message(body, limits);
    REQUIRE(result);
    return std::move(*result);
}

void require_error(jb::core::Result<std::vector<std::string>, Error> const& result,
                   std::string_view                                         code,
                   ErrorCategory                                            category)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK(result.error().category == category);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().detail.empty());
}

auto append_complete(StreamFramer& framer, std::string_view bytes) -> std::vector<std::string>
{
    auto result = framer.append(bytes);
    REQUIRE(result);
    return std::move(*result);
}

} // anonymous namespace

TEST_CASE("framing limits and framer ownership have stable public semantics", "[rpc][framing]")
{
    static_assert(!std::is_copy_constructible_v<StreamFramer>);
    static_assert(!std::is_copy_assignable_v<StreamFramer>);
    static_assert(std::is_nothrow_move_constructible_v<StreamFramer>);
    static_assert(std::is_nothrow_move_assignable_v<StreamFramer>);

    StreamFramer framer;
    CHECK(framer.limits().max_header_bytes == std::size_t{16} * 1024U);
    CHECK(framer.limits().max_body_bytes == std::size_t{1024} * 1024U);
    CHECK(framer.buffered_bytes() == 0U);
}

TEST_CASE("frame_message emits deterministic byte-counted framing", "[rpc][framing]")
{
    auto ordinary = frame_message("body");
    REQUIRE(ordinary);
    CHECK(*ordinary == "Content-Length: 4\r\n\r\nbody");

    auto empty = frame_message("");
    REQUIRE(empty);
    CHECK(*empty == "Content-Length: 0\r\n\r\n");

    auto const binary_body = std::string{"\0\xff\x01", 3};
    auto       binary      = frame_message(binary_body);
    REQUIRE(binary);
    REQUIRE(binary->substr(0, 21U) == "Content-Length: 3\r\n\r\n");
    CHECK(binary->substr(21U) == binary_body);
}

TEST_CASE("frame_message enforces inclusive header and body limits", "[rpc][framing]")
{
    auto exact_body = frame_message("abc", {.max_header_bytes = 21U, .max_body_bytes = 3U});
    REQUIRE(exact_body);

    auto large_body = frame_message("abcd", {.max_header_bytes = 21U, .max_body_bytes = 3U});
    REQUIRE_FALSE(large_body);
    CHECK(large_body.error().code == "rpc.framing.body_too_large");
    CHECK(large_body.error().category == ErrorCategory::ResourceExhausted);

    auto exact_header = frame_message("", {.max_header_bytes = 21U, .max_body_bytes = 0U});
    REQUIRE(exact_header);

    auto large_header = frame_message("", {.max_header_bytes = 20U, .max_body_bytes = 0U});
    REQUIRE_FALSE(large_header);
    CHECK(large_header.error().code == "rpc.framing.header_too_large");
    CHECK(large_header.error().category == ErrorCategory::ResourceExhausted);
}

TEST_CASE("StreamFramer accepts every split point in a frame", "[rpc][framing]")
{
    auto const frame = encoded("fragmented");
    for (auto split = std::size_t{0}; split <= frame.size(); ++split) {
        CAPTURE(split);
        StreamFramer framer;
        auto         bodies = std::vector<std::string>{};

        auto first = framer.append(std::string_view{frame}.substr(0, split));
        REQUIRE(first);
        bodies.insert(bodies.end(), std::make_move_iterator(first->begin()), std::make_move_iterator(first->end()));

        auto second = framer.append(std::string_view{frame}.substr(split));
        REQUIRE(second);
        bodies.insert(bodies.end(), std::make_move_iterator(second->begin()), std::make_move_iterator(second->end()));

        REQUIRE(bodies.size() == 1U);
        CHECK(bodies.front() == "fragmented");
        CHECK(framer.buffered_bytes() == 0U);
    }
}

TEST_CASE("StreamFramer accepts a frame one byte at a time", "[rpc][framing]")
{
    auto const body       = std::string{"bytes"};
    auto const frame      = encoded(body);
    auto const header_end = frame.find("\r\n\r\n");
    REQUIRE(header_end != std::string::npos);
    auto const   header_size = header_end + 4U;
    StreamFramer framer;

    for (auto index = std::size_t{0}; index < frame.size(); ++index) {
        auto result = framer.append(std::string_view{frame}.substr(index, 1U));
        REQUIRE(result);

        if (index + 1U < header_size) {
            CHECK(result->empty());
            CHECK(framer.buffered_bytes() == index + 1U);
        }
        else if (index + 1U < frame.size()) {
            CHECK(result->empty());
            CHECK(framer.buffered_bytes() == index + 1U - header_size);
        }
        else {
            REQUIRE(result->size() == 1U);
            CHECK(result->front() == body);
            CHECK(framer.buffered_bytes() == 0U);
        }
    }
}

TEST_CASE("StreamFramer retains only incomplete header or body bytes", "[rpc][framing]")
{
    StreamFramer framer;

    auto header = framer.append("Content-Length: 4\r\n");
    REQUIRE(header);
    CHECK(header->empty());
    CHECK(framer.buffered_bytes() == 19U);

    auto empty_header = framer.append("");
    REQUIRE(empty_header);
    CHECK(empty_header->empty());
    CHECK(framer.buffered_bytes() == 19U);

    auto partial_body = framer.append("\r\nab");
    REQUIRE(partial_body);
    CHECK(partial_body->empty());
    CHECK(framer.buffered_bytes() == 2U);

    auto empty_body = framer.append("");
    REQUIRE(empty_body);
    CHECK(framer.buffered_bytes() == 2U);

    auto complete = framer.append("cd");
    REQUIRE(complete);
    REQUIRE(complete->size() == 1U);
    CHECK(complete->front() == "abcd");
    CHECK(framer.buffered_bytes() == 0U);
}

TEST_CASE("StreamFramer preserves coalesced message order and following fragments", "[rpc][framing]")
{
    auto const first  = encoded("first");
    auto const second = encoded("");
    auto const third  = encoded("third");

    StreamFramer framer;
    auto         coalesced = append_complete(framer, first + second + third);
    REQUIRE(coalesced == std::vector<std::string>{"first", "", "third"});
    CHECK(framer.buffered_bytes() == 0U);

    auto const fourth = encoded("fourth");
    auto const next   = std::string{"Content-Length: 4\r\n"};
    auto       mixed  = append_complete(framer, fourth + next);
    REQUIRE(mixed == std::vector<std::string>{"fourth"});
    CHECK(framer.buffered_bytes() == next.size());

    auto final = append_complete(framer, "\r\nlast");
    REQUIRE(final == std::vector<std::string>{"last"});
}

TEST_CASE("StreamFramer returns bodies that own opaque input bytes", "[rpc][framing]")
{
    auto body  = std::string{"\0\xff\x02", 3};
    auto frame = encoded(body);

    StreamFramer framer;
    auto         result = framer.append(frame);
    REQUIRE(result);
    REQUIRE(result->size() == 1U);

    frame.assign(frame.size(), 'x');
    CHECK(result->front() == body);
}

TEST_CASE("StreamFramer accepts header casing whitespace and unknown fields", "[rpc][framing]")
{
    auto const accepted = std::array<std::string_view, 4>{
        "content-length: 3\r\n\r\none",
        "CONTENT-LENGTH:\t3 \t\r\n\r\ntwo",
        "Future-Header: future value\r\nContent-Length: 5\r\n\r\nthree",
        "X-Token_~: value\r\nCoNtEnT-LeNgTh: 4\r\n\r\nfour",
    };

    auto const expected = std::array<std::string_view, 4>{"one", "two", "three", "four"};
    for (auto index = std::size_t{0}; index < accepted.size(); ++index) {
        CAPTURE(index);
        StreamFramer framer;
        auto         bodies = append_complete(framer, accepted[index]);
        REQUIRE(bodies.size() == 1U);
        CHECK(bodies.front() == expected[index]);
    }
}

TEST_CASE("StreamFramer accepts precisely parsed UTF-8 content types", "[rpc][framing]")
{
    auto const accepted = std::array<std::string_view, 4>{
        "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\nContent-Length: 0\r\n\r\n",
        "content-type: Application/Json; Charset=UTF8\r\ncontent-length: 0\r\n\r\n",
        "Content-Type: text/plain ; charset = \"Utf-8\"\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/custom; version=1; charset=\"utf8\"; mode=test\r\nContent-Length: 0\r\n\r\n",
    };

    for (auto frame : accepted) {
        CAPTURE(frame);
        StreamFramer framer;
        auto         bodies = append_complete(framer, frame);
        REQUIRE(bodies.size() == 1U);
        CHECK(bodies.front().empty());
    }
}

TEST_CASE("StreamFramer distinguishes unsupported and malformed content types", "[rpc][framing]")
{
    auto const unsupported = std::array<std::string_view, 4>{
        "Content-Type: application/json\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/json; charset=latin1\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/json; charset=utf-8; charset=utf8\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/json; charset=\"\"\r\nContent-Length: 0\r\n\r\n",
    };
    for (auto frame : unsupported) {
        CAPTURE(frame);
        StreamFramer framer;
        require_error(framer.append(frame), "rpc.framing.unsupported_content_type", ErrorCategory::Unsupported);
    }

    auto const malformed = std::array<std::string_view, 4>{
        "Content-Type: application; charset=utf-8\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: /json; charset=utf-8\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/json; =utf-8\r\nContent-Length: 0\r\n\r\n",
        "Content-Type: application/json; charset=\"utf-8\r\nContent-Length: 0\r\n\r\n",
    };
    for (auto frame : malformed) {
        CAPTURE(frame);
        StreamFramer framer;
        require_error(framer.append(frame), "rpc.framing.invalid_header", ErrorCategory::InvalidArgument);
    }
}

TEST_CASE("StreamFramer rejects malformed CRLF and header fields", "[rpc][framing]")
{
    struct Failure {
        std::string input;
        std::string code;
    };

    auto const failures = std::array{
        Failure{.input = "Content-Length: 0\n\n",                             .code = "rpc.framing.invalid_header"        },
        Failure{.input = "Content-Length: 0\rX",                              .code = "rpc.framing.invalid_header"        },
        Failure{.input = ": value\r\nContent-Length: 0\r\n\r\n",              .code = "rpc.framing.invalid_header"        },
        Failure{.input = "Content-Length 0\r\n\r\n",                          .code = "rpc.framing.invalid_header"        },
        Failure{.input = "Content-Length : 0\r\n\r\n",                        .code = "rpc.framing.invalid_header"        },
        Failure{.input = "Content@Length: 0\r\n\r\n",                         .code = "rpc.framing.invalid_header"        },
        Failure{.input = std::string{"X: \x01\r\nContent-Length: 0\r\n\r\n"}, .code = "rpc.framing.invalid_header"        },
        Failure{.input = "Future: SECRET-MARKER\r\n\r\n",                     .code = "rpc.framing.missing_content_length"},
        Failure{.input =
                    "Content-Type: application/json; charset=utf-8\r\nContent-Type: application/json; charset=utf-8\r\n"
                    "Content-Length: 0\r\n\r\n",              .code = "rpc.framing.invalid_header"        },
    };

    for (auto const& failure : failures) {
        CAPTURE(failure.input);
        StreamFramer framer;
        auto         result = framer.append(failure.input);
        require_error(result, failure.code, ErrorCategory::InvalidArgument);
        CHECK(result.error().message.find("SECRET-MARKER") == std::string::npos);
        CHECK(result.error().detail.find("SECRET-MARKER") == std::string::npos);
    }

    StreamFramer fragmented;
    auto         first = fragmented.append("Content-Length: 0\r");
    REQUIRE(first);
    auto second = fragmented.append("\n\r");
    REQUIRE(second);
    auto final = fragmented.append("\n");
    REQUIRE(final);
    REQUIRE(final->size() == 1U);
    CHECK(final->front().empty());
}

TEST_CASE("StreamFramer rejects duplicate Content-Length fields", "[rpc][framing]")
{
    auto const duplicates = std::array<std::string_view, 2>{
        "Content-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "Content-Length: 0\r\nFuture: value\r\ncontent-length: 1\r\n\r\n",
    };

    for (auto frame : duplicates) {
        StreamFramer framer;
        require_error(framer.append(frame), "rpc.framing.duplicate_content_length", ErrorCategory::InvalidArgument);
    }
}

TEST_CASE("StreamFramer validates decimal Content-Length spelling and range", "[rpc][framing]")
{
    auto const invalid = std::array<std::string_view, 8>{"", " ", "+1", "-1", "1 0", "1x", "1.0", "1,2"};
    for (auto value : invalid) {
        CAPTURE(value);
        StreamFramer framer;
        auto const   frame = std::string{"Content-Length:"} + std::string{value} + "\r\n\r\n";
        require_error(framer.append(frame), "rpc.framing.invalid_content_length", ErrorCategory::InvalidArgument);
    }

    auto const valid = std::array<std::string_view, 3>{
        "Content-Length: 0\r\n\r\n",
        "Content-Length: 000\r\n\r\n",
        "Content-Length:\t 0 \t\r\n\r\n",
    };
    for (auto frame : valid) {
        StreamFramer framer;
        auto         bodies = append_complete(framer, frame);
        REQUIRE(bodies.size() == 1U);
        CHECK(bodies.front().empty());
    }

    auto const   maximum = std::to_string(std::numeric_limits<std::size_t>::max());
    StreamFramer maximum_framer({.max_body_bytes = std::numeric_limits<std::size_t>::max()});
    auto         maximum_result = maximum_framer.append("Content-Length: " + maximum + "\r\n\r\n");
    REQUIRE(maximum_result);
    CHECK(maximum_result->empty());
    CHECK(maximum_framer.buffered_bytes() == 0U);

    StreamFramer overflow_framer({.max_body_bytes = std::numeric_limits<std::size_t>::max()});
    require_error(overflow_framer.append("Content-Length: " + maximum + "0\r\n\r\n"),
                  "rpc.framing.invalid_content_length",
                  ErrorCategory::InvalidArgument);
}

TEST_CASE("StreamFramer enforces body limits immediately after the header", "[rpc][framing]")
{
    StreamFramer exact({.max_body_bytes = 3U});
    auto         complete = append_complete(exact, "Content-Length: 3\r\n\r\nabc");
    REQUIRE(complete == std::vector<std::string>{"abc"});

    StreamFramer excessive({.max_body_bytes = 3U});
    auto         failure = excessive.append("Content-Length: 4\r\n\r\n");
    require_error(failure, "rpc.framing.body_too_large", ErrorCategory::ResourceExhausted);
    CHECK(excessive.buffered_bytes() == 0U);
}

TEST_CASE("StreamFramer enforces inclusive terminated and unterminated header limits", "[rpc][framing]")
{
    auto const zero_frame  = std::string{"Content-Length: 0\r\n\r\n"};
    auto const header_size = zero_frame.size();

    StreamFramer exact({.max_header_bytes = header_size, .max_body_bytes = 0U});
    auto         complete = append_complete(exact, zero_frame);
    REQUIRE(complete == std::vector<std::string>{""});

    StreamFramer excessive({.max_header_bytes = header_size - 1U, .max_body_bytes = 0U});
    require_error(excessive.append(zero_frame), "rpc.framing.header_too_large", ErrorCategory::ResourceExhausted);

    StreamFramer unterminated({.max_header_bytes = 8U});
    auto         boundary = unterminated.append("12345678");
    REQUIRE(boundary);
    CHECK(boundary->empty());
    CHECK(unterminated.buffered_bytes() == 8U);
    require_error(unterminated.append("9"), "rpc.framing.header_too_large", ErrorCategory::ResourceExhausted);
    CHECK(unterminated.buffered_bytes() == 0U);
}

TEST_CASE("StreamFramer remains poisoned with its first error until reset", "[rpc][framing]")
{
    StreamFramer framer({.max_header_bytes = 128U, .max_body_bytes = 16U});
    auto         first = framer.append("Content-Length: invalid\r\n\r\n");
    require_error(first, "rpc.framing.invalid_content_length", ErrorCategory::InvalidArgument);
    CHECK(framer.buffered_bytes() == 0U);

    auto second = framer.append(encoded("valid"));
    REQUIRE_FALSE(second);
    CHECK(second.error() == first.error());
    CHECK(framer.buffered_bytes() == 0U);

    auto empty = framer.append("");
    REQUIRE_FALSE(empty);
    CHECK(empty.error() == first.error());

    framer.reset();
    CHECK(framer.buffered_bytes() == 0U);
    CHECK(framer.limits().max_header_bytes == 128U);
    CHECK(framer.limits().max_body_bytes == 16U);
    auto recovered = append_complete(framer, encoded("valid"));
    REQUIRE(recovered == std::vector<std::string>{"valid"});
}

TEST_CASE("StreamFramer reset discards successful partial input", "[rpc][framing]")
{
    StreamFramer framer;
    auto         partial = framer.append("Content-Length: 4\r\n\r\nab");
    REQUIRE(partial);
    CHECK(framer.buffered_bytes() == 2U);

    framer.reset();
    CHECK(framer.buffered_bytes() == 0U);
    auto complete = append_complete(framer, encoded("new"));
    REQUIRE(complete == std::vector<std::string>{"new"});
}

TEST_CASE("StreamFramer does not return partial success before a later framing error", "[rpc][framing]")
{
    StreamFramer framer;
    auto         result = framer.append(encoded("complete") + "Content-Length: invalid\r\n\r\n");
    require_error(result, "rpc.framing.invalid_content_length", ErrorCategory::InvalidArgument);
    CHECK(framer.buffered_bytes() == 0U);
}

TEST_CASE("StreamFramer moves partial and poisoned state", "[rpc][framing]")
{
    StreamFramer partial;
    auto         prefix = partial.append("Content-Length: 4\r\n\r\nab");
    REQUIRE(prefix);
    CHECK(partial.buffered_bytes() == 2U);

    StreamFramer moved{std::move(partial)};
    auto         complete = append_complete(moved, "cd");
    REQUIRE(complete == std::vector<std::string>{"abcd"});

    StreamFramer poisoned;
    auto         original = poisoned.append("Content-Length: bad\r\n\r\n");
    REQUIRE_FALSE(original);

    StreamFramer assigned;
    assigned        = std::move(poisoned);
    auto after_move = assigned.append(encoded("ignored"));
    REQUIRE_FALSE(after_move);
    CHECK(after_move.error() == original.error());

    assigned.reset();
    auto recovered = append_complete(assigned, encoded("ready"));
    REQUIRE(recovered == std::vector<std::string>{"ready"});
}
