#include "management.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<jb::jobu::ManagementService>);
static_assert(!std::is_move_constructible_v<jb::jobu::ManagementService>);
static_assert(std::is_same_v<jb::jobu::QueueSelector, std::variant<jb::core::Uuid, std::string>>);

int main()
{
    jb::jobu::PageOptions const page;
    return page.limit == 100 ? 0 : 1;
}
