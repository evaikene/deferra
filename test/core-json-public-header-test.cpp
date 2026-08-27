#include "json.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

using ParseResult     = jb::core::Result<jb::core::JsonValue, jb::core::Error>;
using SerializeResult = jb::core::Result<std::string, jb::core::Error>;

static_assert(std::is_same_v<decltype(&jb::core::parse_json), ParseResult (*)(std::string_view, jb::core::JsonLimits)>);
static_assert(std::is_same_v<decltype(&jb::core::serialize_json), SerializeResult (*)(jb::core::JsonValue const&)>);
static_assert(std::is_default_constructible_v<jb::core::JsonNull>);

auto main() -> int
{
    auto value = jb::core::JsonValue{};
    value.data = jb::core::JsonValue::Object{};

    auto limits      = jb::core::JsonLimits{};
    limits.max_depth = std::size_t{8};

    return value.is_object() && limits.max_depth == std::size_t{8} ? 0 : 1;
}
