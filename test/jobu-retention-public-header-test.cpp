#include "retention.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_copy_constructible_v<jb::jobu::RetentionOptions>);
static_assert(std::is_copy_constructible_v<jb::jobu::RetentionPurgeCounts>);
static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::RetentionService>);
static_assert(!std::is_copy_constructible_v<jb::jobu::RetentionService>);
static_assert(!std::is_move_constructible_v<jb::jobu::RetentionService>);
static_assert(std::is_constructible_v<jb::jobu::RetentionService,
                                      jb::db::Database&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::core::TimeSource&>);
static_assert(noexcept(std::declval<jb::jobu::RetentionService&>().stop()));

auto main() -> int
{
    return 0;
}
