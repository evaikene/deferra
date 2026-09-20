#include "recovery.hpp"

#include <type_traits>

static_assert(std::is_default_constructible_v<jb::jobu::RecoveryOptions>);
static_assert(std::is_copy_constructible_v<jb::jobu::RecoveryReport>);
static_assert(jb::jobu::RecoveryOptions{}.scan_batch_size == 256);
static_assert(jb::jobu::RecoveryReport{}.interrupted_attempts == 0);

using RecoveryFunction =
    jb::core::Result<jb::jobu::RecoveryReport, jb::core::Error> (*)(jb::db::Database&,
                                                                    jb::jobu::AttributeRegistry const&,
                                                                    jb::jobu::CronEngine const&,
                                                                    jb::core::UuidGenerator&,
                                                                    jb::core::TimeSource&,
                                                                    jb::jobu::RecoveryOptions);
static_assert(std::is_same_v<decltype(&jb::jobu::recover_startup), RecoveryFunction>);

auto main() -> int
{
    return 0;
}
