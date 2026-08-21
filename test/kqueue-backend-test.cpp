#include "event_loop_backend_kqueue_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <functional>
#include <vector>

using namespace jb::core;

namespace {

struct FilterChange {
    FdEvent event;
    bool    enable;
};

struct ScriptedFilterChanges {
    auto operator()(FdEvent event, bool enable) -> bool
    {
        calls.push_back({.event = event, .enable = enable});
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

void check_change(FilterChange const& change, FdEvent event, bool enable)
{
    CHECK(change.event == event);
    CHECK(change.enable == enable);
}

} // anonymous namespace

TEST_CASE("Kqueue filter transition applies the requested mask", "[core][kqueue]")
{
    ScriptedFilterChanges changes;

    auto const result =
        jb::core::priv::transition_kqueue_filters({}, FdEvents{FdEvent::Read, FdEvent::Write}, std::ref(changes));
    auto const expected = FdEvents{FdEvent::Read, FdEvent::Write};

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::Applied);
    CHECK(result.events.bits() == expected.bits());
    REQUIRE(changes.calls.size() == 2);
    check_change(changes.calls[0], FdEvent::Read, true);
    check_change(changes.calls[1], FdEvent::Write, true);
}

TEST_CASE("Kqueue filter transition does nothing for an unchanged mask", "[core][kqueue]")
{
    ScriptedFilterChanges changes;
    auto const            requested = FdEvents{FdEvent::Read, FdEvent::Write};

    auto const result = jb::core::priv::transition_kqueue_filters(requested, requested, std::ref(changes));

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::Applied);
    CHECK(result.events.bits() == requested.bits());
    CHECK(changes.calls.empty());
}

TEST_CASE("Kqueue filter transition rolls back a partial addition", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const result =
        jb::core::priv::transition_kqueue_filters({}, FdEvents{FdEvent::Read, FdEvent::Write}, std::ref(changes));

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::FailedRolledBack);
    CHECK(result.events.none());
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, true);
    check_change(changes.calls[1], FdEvent::Write, true);
    check_change(changes.calls[2], FdEvent::Read, false);
}

TEST_CASE("Kqueue filter transition rolls back a failed replacement", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const result = jb::core::priv::transition_kqueue_filters(FdEvent::Read, FdEvent::Write, std::ref(changes));

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::FailedRolledBack);
    CHECK(result.events.bits() == FdEvents{FdEvent::Read}.bits());
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, false);
    check_change(changes.calls[1], FdEvent::Write, true);
    check_change(changes.calls[2], FdEvent::Read, true);
}

TEST_CASE("Kqueue filter transition rolls back a partial removal", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, true}
    };

    auto const result =
        jb::core::priv::transition_kqueue_filters(FdEvents{FdEvent::Read, FdEvent::Write}, {}, std::ref(changes));
    auto const expected = FdEvents{FdEvent::Read, FdEvent::Write};

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::FailedRolledBack);
    CHECK(result.events.bits() == expected.bits());
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, false);
    check_change(changes.calls[1], FdEvent::Write, false);
    check_change(changes.calls[2], FdEvent::Read, true);
}

TEST_CASE("Kqueue filter transition reports rollback failure", "[core][kqueue]")
{
    ScriptedFilterChanges changes{
        .results = {true, false, false}
    };

    auto const result =
        jb::core::priv::transition_kqueue_filters({}, FdEvents{FdEvent::Read, FdEvent::Write}, std::ref(changes));

    CHECK(result.status == jb::core::priv::KqueueTransitionStatus::RollbackFailed);
    CHECK(result.events.bits() == FdEvents{FdEvent::Read}.bits());
    REQUIRE(changes.calls.size() == 3);
    check_change(changes.calls[0], FdEvent::Read, true);
    check_change(changes.calls[1], FdEvent::Write, true);
    check_change(changes.calls[2], FdEvent::Read, false);
}
