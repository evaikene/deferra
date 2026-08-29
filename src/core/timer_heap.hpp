#pragma once

#include "event_loop_types.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace jb::core::priv {

struct EventLoopTestAccess;

struct TimerEntry {
    TimerHandle::id_t id;
    Task              callback;
    TimePoint         deadline;
    Duration          interval;
};

struct EarliestDeadline {
    auto operator()(TimerEntry const& a, TimerEntry const& b) -> bool { return a.deadline > b.deadline; }
};

class TimerHeap {
public:

    TimerHeap() = default;

    /// Constructor that reserves space for the given number of timers.
    explicit TimerHeap(std::size_t reserve_count) { reserve(reserve_count); }

    /// Reserve space for the given number of timers. This is an optimization to
    /// avoid reallocations if the number of timers is known in advance.
    void reserve(std::size_t count)
    {
        _heap.reserve(count);
    }

    /// Schedule a timer to run at the given deadline, optionally repeating at the given interval.
    auto start(Task cb, TimePoint deadline, Duration interval = {}) -> TimerHandle
    {
        auto id = _next_id++;
        push_heap({.id=id, .callback=std::move(cb), .deadline=deadline, .interval=interval});
        _timers.insert(id);

        return {id};
    }

    /// Cancel a timer by its handle. If the timer is already executed or cancelled, this is a no-op.
    void cancel(TimerHandle h)
    {
        _timers.erase(h.id);
    }

    /// Returns the deadline of the next live timer, or nullopt
    auto next_deadline() -> std::optional<TimePoint>
    {
        while (!_heap.empty()) {
            if (!discard_cancelled_top()) {
                return std::nullopt;
            }

            auto const& top = _heap.front();
            return top.deadline;
        }

        return std::nullopt;
    }

    /// Pop and invoke all the timers whose deadline <= now
    void fire_expired(TimePoint now)
    {
        // fire all the expired timers
        while (true) {
            if (!discard_cancelled_top()) {
                break;
            }

            // check for deadline
            auto const& top = _heap.front();
            if (top.deadline > now) {
                break;
            }

            // pop the entry and fire it
            auto entry = pop_heap();
            entry.callback();

            // check for repeating timers that were not cancelled by the callback
            bool repeating_timer = (entry.interval.count() > 0) && (_timers.contains(entry.id));
            if (repeating_timer) {
                // re-arm relative to the original deadline
                entry.deadline += entry.interval;
                push_heap(std::move(entry));
            }
            else {
                _timers.erase(entry.id);
            }
        }
    }

private:

    friend struct EventLoopTestAccess;

    std::vector<TimerEntry>               _heap;
    std::unordered_set<TimerHandle::id_t> _timers;
    TimerHandle::id_t                     _next_id{1};

    /// Discard cancelled timers from the top of the heap.
    /// @return true if there are any live timers left after discarding.
    auto discard_cancelled_top() -> bool
    {
        while (!_heap.empty()) {
            auto const& top = _heap.front();
            if (_timers.contains(top.id)) {
                break;
            }

            pop_heap();
        }

        return !_heap.empty();
    }

    void push_heap(TimerEntry&& e)
    {
        _heap.push_back(std::move(e));
        std::ranges::push_heap(_heap, EarliestDeadline{});
    }

    auto pop_heap() -> TimerEntry
    {
        std::ranges::pop_heap(_heap, EarliestDeadline{});
        auto e = std::move(_heap.back());
        _heap.pop_back();

        return e;
    }
};

} //  namespace jb::core::priv
