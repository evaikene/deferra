#include "queue.hpp" // IWYU pragma: keep for public header self-containment coverage

#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<jb::jobu::QueueRevision, std::uint64_t>);
static_assert(std::is_same_v<decltype(jb::jobu::Queue::revision), jb::jobu::QueueRevision>);

auto main() -> int
{
    return jb::jobu::Queue{}.revision == 1 ? 0 : 1;
}
