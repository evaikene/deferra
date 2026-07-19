#include "value.hpp"

namespace jb::db {

auto make_text(std::string_view value) -> Value
{
    return std::string{value};
}

auto make_blob(jb::core::ByteView value) -> Value
{
    return jb::core::ByteBuffer{value.begin(), value.end()};
}

} // namespace jb::db
