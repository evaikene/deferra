#include "framing.hpp"
#include "json.hpp"
#include "rpc.hpp"

#include <type_traits>
#include <utility>

auto main() -> int
{
    static_assert(!std::is_copy_constructible_v<jb::rpc::StreamFramer>);
    static_assert(std::is_nothrow_move_constructible_v<jb::rpc::StreamFramer>);

    jb::rpc::StreamFramer framer;
    jb::rpc::StreamFramer moved{std::move(framer)};
    return moved.buffered_bytes() == 0U ? 0 : 1;
}
