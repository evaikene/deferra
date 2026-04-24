#pragma once

#include "event_loop_types.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace jb::core::priv {

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
        std::lock_guard lock{_mx};
        _heap.reserve(count);
    }

    /// Schedule a timer to run at the given deadline, optionally repeating at the given interval.
    auto start(Task cb, TimePoint deadline, Duration interval = {}) -> TimerHandle
    {
        auto id = _next_id++;
        {
            std::lock_guard lock{_mx};
            push_heap({id, std::move(cb), deadline, interval});
            _timers.insert(id);
        }
        return {id};
    }

    /// Cancel a timer by its handle. If the timer is already executed or cancelled, this is a no-op.
    void cancel(TimerHandle h)
    {
        std::lock_guard lock{_mx};
        _timers.erase(h.id);
    }

    /// Returns the deadline of the next live timer, or nullopt
    auto next_deadline() -> std::optional<TimePoint>
    {
        std::lock_guard lock{_mx};

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
        // collect all the expired timers in a separate queue to minimize the time we hold the lock
        std::queue<TimerEntry> local;
        {
            std::lock_guard lock{_mx};

            while (true) {
                if (!discard_cancelled_top()) {
                    break;
                }

                auto const& top = _heap.front();
                if (top.deadline > now) {
                    break;
                }

                local.push(pop_heap());
            }
        }

        // fire the expired timers without holding the lock
        while (!local.empty()) {
            auto entry = std::move(local.front());
            local.pop();

            entry.callback(); // fire

            bool repeating_timer = entry.interval.count() > 0;
            {
                std::lock_guard lock{_mx};
                repeating_timer &= (_timers.count(entry.id) > 0);
                if (!repeating_timer) {
                    _timers.erase(entry.id);
                }
                else {
                    // repeating: re-arm relative to original deadline
                    entry.deadline += entry.interval;

                    _timers.insert(entry.id);
                    push_heap(std::move(entry));
                }
            }
        }
    }

private:

    mutable std::mutex                    _mx;
    std::vector<TimerEntry>               _heap;
    std::unordered_set<TimerHandle::id_t> _timers;
    std::atomic<TimerHandle::id_t>        _next_id{1};

    /// Discard cancelled timers from the top of the heap.
    /// @return true if there are any live timers left after discarding.
    auto discard_cancelled_top() -> bool
    {
        while (!_heap.empty()) {
            auto const& top = _heap.front();
            if (_timers.count(top.id) > 0) {
                break;
            }

            pop_heap();
        }

        return !_heap.empty();
    }

    void push_heap(TimerEntry&& e)
    {
        _heap.push_back(std::move(e));
        std::push_heap(_heap.begin(), _heap.end(), EarliestDeadline{});
    }

    auto pop_heap() -> TimerEntry
    {
        std::pop_heap(_heap.begin(), _heap.end(), EarliestDeadline{});
        auto e = std::move(_heap.back());
        _heap.pop_back();

        return e;
    }
};

} //  namespace jb::core::priv
