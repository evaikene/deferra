#pragma once

#include "event_loop_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace jb::core::priv {

/// Outcome of a transactional kqueue fd-filter transition.
enum class KqueueTransitionStatus : std::uint8_t {
    Applied,
    FailedRolledBack,
    RollbackFailed,
};

/// Result of a transactional kqueue fd-filter transition.
struct KqueueTransitionResult {
    KqueueTransitionStatus status;
    FdEvents               events;
};

/// Applies a requested fd-filter mask and restores the original mask on failure.
///
/// The callback receives the filter and whether it should be enabled. A false
/// callback result means that operation did not change the native filter state.
/// Successfully applied changes are rolled back in reverse order.
template <typename ApplyFilter>
auto transition_kqueue_filters(FdEvents current, FdEvents requested, ApplyFilter&& apply_filter)
    -> KqueueTransitionResult
{
    static constexpr std::array kFilters{FdEvent::Read, FdEvent::Write};

    auto const                           original = current;
    std::array<FdEvent, kFilters.size()> changed_filters;
    std::size_t                          changed_count{0};

    for (auto const filter : kFilters) {
        auto const enable = requested.test(filter);
        if (current.test(filter) == enable) {
            continue;
        }

        if (!apply_filter(filter, enable)) {
            auto rollback_succeeded = true;
            while (changed_count > 0) {
                auto const changed_filter = changed_filters[--changed_count];
                auto const restore        = original.test(changed_filter);
                if (!apply_filter(changed_filter, restore)) {
                    rollback_succeeded = false;
                    continue;
                }

                if (restore) {
                    current.set(changed_filter);
                }
                else {
                    current.clear(changed_filter);
                }
            }

            return {
                .status = rollback_succeeded ? KqueueTransitionStatus::FailedRolledBack
                                             : KqueueTransitionStatus::RollbackFailed,
                .events = current,
            };
        }

        if (enable) {
            current.set(filter);
        }
        else {
            current.clear(filter);
        }
        changed_filters[changed_count++] = filter;
    }

    return {.status = KqueueTransitionStatus::Applied, .events = current};
}

} // namespace jb::core::priv
