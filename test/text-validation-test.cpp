#include "text_validation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("core text validation accepts complete UTF-8 independently of control policy", "[core][text]")
{
    CHECK(jb::core::is_valid_utf8(""));
    CHECK(jb::core::is_valid_utf8("plain ASCII"));
    CHECK(jb::core::is_valid_utf8("\xC2\xA3\xE2\x82\xAC\xF0\x9F\x98\x80"));
    CHECK(jb::core::is_valid_utf8(std::string_view{"a\0b", 3U}));
}

TEST_CASE("core text validation rejects malformed UTF-8", "[core][text]")
{
    for (auto const* invalid : {"\x80", "\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF"}) {
        CAPTURE(invalid);
        CHECK_FALSE(jb::core::is_valid_utf8(invalid));
    }
}

TEST_CASE("core text validation detects ASCII controls in arbitrary bytes", "[core][text]")
{
    CHECK_FALSE(jb::core::has_ascii_control(""));
    CHECK_FALSE(jb::core::has_ascii_control("plain ASCII"));
    CHECK_FALSE(jb::core::has_ascii_control("\xC2\xA3\xE2\x82\xAC"));
    CHECK(jb::core::has_ascii_control(std::string_view{"a\0b", 3U}));
    CHECK(jb::core::has_ascii_control("a\nb"));
    CHECK(jb::core::has_ascii_control("a\x7F"));
}
