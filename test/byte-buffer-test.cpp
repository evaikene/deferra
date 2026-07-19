#include "byte_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace jb::core;

TEST_CASE("Byte conversions preserve all bytes", "[core][bytes]")
{
    auto const text = std::string{"text\0\xff", 6};
    auto const view = as_bytes(text);

    REQUIRE(view.size() == text.size());
    CHECK(as_string_view(view) == text);
}
