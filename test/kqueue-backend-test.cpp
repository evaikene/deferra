#include "event_loop_backend_kqueue_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <functional>
#include <vector>

using namespace jb::core;

namespace {

using jb::core::priv::KqueueFdRegistration;
using jb::core::priv::KqueueFilterMode;
using jb::core::priv::KqueueTransitionStatus;

struct FilterChange {
    FdEvent          event;
    KqueueFilterMode mode;
};

struct ScriptedFilterChanges {
    auto operator()(FdEvent event, KqueueFilterMode mode) -> bool
    {
        calls.push_back({.event = event, .mode = mode});
        if (results.empty()) {
            return true;
        }

        auto const result = results.front();
        results.pop_front();
        return result;
    }

    std::deque<bool>          results;
    std::vector<FilterChange> calls;
};

constexpr auto registration(FdEvents events, FdTriggerMode trigger_mode) -> KqueueFdRegistration
{
    return {
        .events       = events,
        .trigger_mode = trigger_mode,
    };
}

void check_change(FilterChange const& change, FdEvent event, KqueueFilterMode mode)
{
    CHECK(change.event == event);
    CHECK(change.mode == mode);
}

} // anonymous namespace

TEST_CASE("Kqueue filter transition applies an initial edge registration", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration({}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 2);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Edge);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition applies an initial level registration", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration({}, FdTriggerMode::Level),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Level),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 2);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Level);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Level);
}

TEST_CASE("Kqueue filter transition does nothing for an unchanged registration", "[core][kqueue]")
{
    for (auto const trigger_mode : {FdTriggerMode::Edge, FdTriggerMode::Level}) {
        ScriptedFilterChanges changes;
        auto const            requested = registration(FdEvents{FdEvent::Read, FdEvent::Write}, trigger_mode);

        auto const status = jb::core::priv::transition_kqueue_filters(requested, requested, std::ref(changes));

        CHECK(status == KqueueTransitionStatus::Applied);
        CHECK(changes.calls.empty());
    }
}

TEST_CASE("Kqueue filter transition changes edge filters to level", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Level),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 4);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Read, KqueueFilterMode::Level);
    check_change(changes.calls[2], FdEvent::Write, KqueueFilterMode::Disabled);
    check_change(changes.calls[3], FdEvent::Write, KqueueFilterMode::Level);
}

TEST_CASE("Kqueue filter transition changes level filters to edge", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Level),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 4);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Read, KqueueFilterMode::Edge);
    check_change(changes.calls[2], FdEvent::Write, KqueueFilterMode::Disabled);
    check_change(changes.calls[3], FdEvent::Write, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition adds one filter without changing mode", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvent::Read, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 1);
    check_change(changes.calls[0], FdEvent::Write, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition removes one filter without changing mode", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        registration(FdEvent::Read, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 1);
    check_change(changes.calls[0], FdEvent::Write, KqueueFilterMode::Disabled);
}

TEST_CASE("Kqueue filter transition changes mask and mode together", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const status = jb::core::priv::transition_kqueue_filters(registration(FdEvent::Read, FdTriggerMode::Edge),
                                                                  registration(FdEvent::Write, FdTriggerMode::Level),
                                                                  std::ref(changes));

    CHECK(status == KqueueTransitionStatus::Applied);
    REQUIRE(changes.calls.size() == 2);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Level);
}

TEST_CASE("Kqueue filter transition restores a mode when its replacement add fails", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Level),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::FailedRolledBack);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Read, KqueueFilterMode::Level);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition rolls back completed and partial mode changes", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, true, true, false, true, true, true}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Level),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::FailedRolledBack);
    REQUIRE(changes.calls.size() == 7);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Read, KqueueFilterMode::Level);
    check_change(changes.calls[2], FdEvent::Write, KqueueFilterMode::Disabled);
    check_change(changes.calls[3], FdEvent::Write, KqueueFilterMode::Level);
    check_change(changes.calls[4], FdEvent::Write, KqueueFilterMode::Edge);
    check_change(changes.calls[5], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[6], FdEvent::Read, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition reports failure restoring a deleted mode", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, false}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(registration(FdEvent::Read, FdTriggerMode::Edge),
                                                                  registration(FdEvent::Read, FdTriggerMode::Level),
                                                                  std::ref(changes));

    CHECK(status == KqueueTransitionStatus::RollbackFailed);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Read, KqueueFilterMode::Level);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition rolls back a mixed transition", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(registration(FdEvent::Read, FdTriggerMode::Edge),
                                                                  registration(FdEvent::Write, FdTriggerMode::Level),
                                                                  std::ref(changes));

    CHECK(status == KqueueTransitionStatus::FailedRolledBack);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Level);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition rolls back a partial addition", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration({}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::FailedRolledBack);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Edge);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Edge);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Disabled);
}

TEST_CASE("Kqueue filter transition rolls back a partial removal", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        registration({}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::FailedRolledBack);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Disabled);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Disabled);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Edge);
}

TEST_CASE("Kqueue filter transition reports rollback failure", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, false}
    };

    auto const status = jb::core::priv::transition_kqueue_filters(
        registration({}, FdTriggerMode::Edge),
        registration(FdEvents{FdEvent::Read, FdEvent::Write}, FdTriggerMode::Edge),
        std::ref(changes));

    CHECK(status == KqueueTransitionStatus::RollbackFailed);
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, KqueueFilterMode::Edge);
    check_change(changes.calls[1], FdEvent::Write, KqueueFilterMode::Edge);
    check_change(changes.calls[2], FdEvent::Read, KqueueFilterMode::Disabled);
}
