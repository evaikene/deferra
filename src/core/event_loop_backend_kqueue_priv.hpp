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
    /// Native state could not be restored or its prior state was unknown; the backend must become unusable.
    RollbackFailed,
};

/// Complete kqueue registration represented by EventLoop.
struct KqueueFdRegistration {
    FdEvents      events;
    FdTriggerMode trigger_mode{FdTriggerMode::Edge};
};

/// Native state of one kqueue read or write filter.
enum class KqueueFilterMode : std::uint8_t {
    Disabled,
    Edge,
    Level,
};

/// Returns the native mode for one filter in a complete registration.
constexpr auto kqueue_filter_mode(KqueueFdRegistration const& registration, FdEvent filter) noexcept -> KqueueFilterMode
{
    if (!registration.events.test(filter)) {
        return KqueueFilterMode::Disabled;
    }
    return registration.trigger_mode == FdTriggerMode::Edge ? KqueueFilterMode::Edge : KqueueFilterMode::Level;
}

/// Applies a complete kqueue fd-filter registration and restores known original state on failure.
///
/// The callback receives the filter and requested native mode. A false callback
/// result means that operation did not change the native filter state. Successfully
/// applied primitive changes are journaled and rolled back in reverse order.
/// When refresh_enabled is true, reapply unchanged enabled filters too: a retained
/// registration may refer to a closed/reused descriptor whose native filters are gone.
/// A partial refresh of an existing registration returns RollbackFailed without replaying
/// its logical modes: those modes do not establish which native filters existed before the refresh.
template <typename ApplyFilter>
auto transition_kqueue_filters(KqueueFdRegistration const& current,
                               KqueueFdRegistration const& requested,
                               ApplyFilter&&               apply_filter,
                               bool                        refresh_enabled = false) -> KqueueTransitionStatus
{
    static constexpr std::array kFilters{FdEvent::Read, FdEvent::Write};

    struct AppliedChange {
        FdEvent          filter;
        KqueueFilterMode previous_mode;
    };

    std::array<AppliedChange, kFilters.size() * 2U> applied_changes;
    std::size_t                                     applied_count{0};

    auto apply_change = [&](FdEvent filter, KqueueFilterMode previous_mode, KqueueFilterMode requested_mode) {
        if (!apply_filter(filter, requested_mode)) {
            return false;
        }
        applied_changes[applied_count++] = {
            .filter        = filter,
            .previous_mode = previous_mode,
        };
        return true;
    };

    auto transition_succeeded = true;

    for (auto const filter : kFilters) {
        auto const current_mode   = kqueue_filter_mode(current, filter);
        auto const requested_mode = kqueue_filter_mode(requested, filter);
        if (current_mode == requested_mode) {
            if (refresh_enabled && requested_mode != KqueueFilterMode::Disabled &&
                !apply_change(filter, current_mode, requested_mode)) {
                transition_succeeded = false;
                break;
            }
            continue;
        }

        auto const changes_enabled_mode =
            current_mode != KqueueFilterMode::Disabled && requested_mode != KqueueFilterMode::Disabled;
        if (changes_enabled_mode && (!apply_change(filter, current_mode, KqueueFilterMode::Disabled) ||
                                     !apply_change(filter, KqueueFilterMode::Disabled, requested_mode))) {
            transition_succeeded = false;
            break;
        }
        if (!changes_enabled_mode && !apply_change(filter, current_mode, requested_mode)) {
            transition_succeeded = false;
            break;
        }
    }

    if (transition_succeeded) {
        return KqueueTransitionStatus::Applied;
    }

    if (refresh_enabled && applied_count > 0 &&
        (current.events.test(FdEvent::Read) || current.events.test(FdEvent::Write))) {
        // Replaying logical modes could install filters that were absent after descriptor reuse and
        // expose the old callback to a different descriptor. Stop the backend instead of claiming rollback.
        return KqueueTransitionStatus::RollbackFailed;
    }

    auto rollback_succeeded = true;
    while (applied_count > 0) {
        auto const& change = applied_changes[--applied_count];
        if (!apply_filter(change.filter, change.previous_mode)) {
            rollback_succeeded = false;
        }
    }

    return rollback_succeeded ? KqueueTransitionStatus::FailedRolledBack : KqueueTransitionStatus::RollbackFailed;
}

} // namespace jb::core::priv
