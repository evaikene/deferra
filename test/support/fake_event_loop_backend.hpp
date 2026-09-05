#pragma once

#include "error.hpp"
#include "event_loop.hpp"
#include "event_loop_backend.hpp"
#include "result.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace jb::core::priv {

struct FakeFdRegistration {
    int           fd;
    FdEvents      events;
    FdTriggerMode trigger_mode;
};

/// Deterministic event-loop backend used by direct EventLoop tests.
class FakeEventLoopBackend final : public Backend {
public:

    auto add_fd(int fd, FdEvents events, FdTriggerMode trigger_mode) -> bool override
    {
        ++add_fd_calls;
        last_added_fd           = fd;
        last_added_events       = events;
        last_added_trigger_mode = trigger_mode;
        add_fd_history.push_back({
            .fd           = fd,
            .events       = events,
            .trigger_mode = trigger_mode,
        });
        if (!add_fd_results.empty()) {
            auto const result = add_fd_results.front();
            add_fd_results.pop_front();
            return result;
        }
        return add_fd_result;
    }

    auto remove_fd(int fd) -> bool override
    {
        ++remove_fd_calls;
        last_removed_fd = fd;
        if (!remove_fd_results.empty()) {
            auto const result = remove_fd_results.front();
            remove_fd_results.pop_front();
            return result;
        }
        return remove_fd_result;
    }

    auto poll(ReadyEvent* out, int max_events, int timeout_ms) -> int override
    {
        ++poll_calls;
        last_timeout_ms = timeout_ms;

        if (poll_result.has_value()) {
            return *poll_result;
        }

        auto const count = std::min<std::size_t>(ready_events.size(), static_cast<std::size_t>(max_events));
        std::copy_n(ready_events.begin(), count, out);
        ready_events.erase(ready_events.begin(), ready_events.begin() + static_cast<std::ptrdiff_t>(count));
        return static_cast<int>(count);
    }

    auto add_process(std::int64_t process_id) -> ProcessRegistrationResult override
    {
        ++add_process_calls;
        last_added_process = process_id;
        if (!add_process_results.empty()) {
            auto const result = add_process_results.front();
            add_process_results.pop_front();
            return result;
        }
        return add_process_result;
    }

    auto remove_process(std::int64_t process_id) -> bool override
    {
        ++remove_process_calls;
        last_removed_process = process_id;
        if (!remove_process_results.empty()) {
            auto const result = remove_process_results.front();
            remove_process_results.pop_front();
            return result;
        }
        return remove_process_result;
    }

    auto wakeup() -> bool override
    {
        ++wakeup_calls;
        return wakeup_result;
    }

    bool                                  add_fd_result{true};
    bool                                  remove_fd_result{true};
    bool                                  wakeup_result{true};
    std::deque<bool>                      add_fd_results;
    std::deque<bool>                      remove_fd_results;
    std::optional<int>                    poll_result;
    std::vector<ReadyEvent>               ready_events;
    std::vector<FakeFdRegistration>       add_fd_history;
    ProcessRegistrationResult             add_process_result{ProcessRegistrationResult::Added};
    bool                                  remove_process_result{true};
    std::deque<ProcessRegistrationResult> add_process_results;
    std::deque<bool>                      remove_process_results;
    int                                   add_process_calls{0};
    int                                   remove_process_calls{0};
    std::int64_t                          last_added_process{-1};
    std::int64_t                          last_removed_process{-1};

    int           add_fd_calls{0};
    int           remove_fd_calls{0};
    int           poll_calls{0};
    int           wakeup_calls{0};
    int           last_added_fd{-1};
    FdEvents      last_added_events;
    FdTriggerMode last_added_trigger_mode{FdTriggerMode::Edge};
    int           last_removed_fd{-1};
    int           last_timeout_ms{0};
};

struct EventLoopFdRegistration {
    FdEvents      events;
    FdTriggerMode trigger_mode;
};

/// Private constructor access for EventLoop backend-injection tests.
struct EventLoopTestAccess {
    static auto make_event_loop(std::unique_ptr<Backend> backend) -> std::unique_ptr<EventLoop>
    {
        return std::unique_ptr<EventLoop>{new EventLoop(std::move(backend))};
    }

    static auto fd_callback(EventLoop const& loop, int fd) -> FdCallback
    {
        auto const watch = loop._watchers.find(fd);
        return watch == loop._watchers.end() ? FdCallback{} : watch->second.callback;
    }

    static auto fd_registration(EventLoop const& loop, int fd) -> std::optional<EventLoopFdRegistration>
    {
        auto const watch = loop._watchers.find(fd);
        if (watch == loop._watchers.end()) {
            return std::nullopt;
        }
        return EventLoopFdRegistration{
            .events       = watch->second.events,
            .trigger_mode = watch->second.trigger_mode,
        };
    }

    static auto active_timer_count(EventLoop const& loop) -> std::size_t { return loop._timers._timers.size(); }

    static auto watch_process(EventLoop& loop, std::int64_t process_id, Task callback) -> Result<void, Error>
    {
        return loop.watch_process(process_id, std::move(callback));
    }

    static auto unwatch_process(EventLoop& loop, std::int64_t process_id) -> bool
    {
        return loop.unwatch_process(process_id);
    }

    static auto process_callback(EventLoop const& loop, std::int64_t process_id) -> Task
    {
        auto const entry = loop._process_watchers.find(process_id);
        return entry == loop._process_watchers.end() ? Task{} : entry->second;
    }

    static auto active_process_count(EventLoop const& loop) -> std::size_t { return loop._process_watchers.size(); }
};

/// Owned fake backend and its EventLoop test wrapper.
struct FakeEventLoop {
    std::unique_ptr<EventLoop> loop;
    FakeEventLoopBackend*      backend;
};

inline auto make_fake_event_loop() -> FakeEventLoop
{
    auto  backend = std::make_unique<FakeEventLoopBackend>();
    auto* ptr     = backend.get();
    return {.loop = EventLoopTestAccess::make_event_loop(std::move(backend)), .backend = ptr};
}

/// Installs an EventLoop in the current ThreadCtx for one lexical scope.
class ScopedCurrentEventLoop final {
public:

    explicit ScopedCurrentEventLoop(EventLoop* event_loop)
        : _thread_ctx(ThreadCtx::current())
        , _previous(_thread_ctx->event_loop())
    {
        _thread_ctx->set_event_loop(event_loop);
    }

    ~ScopedCurrentEventLoop() { _thread_ctx->set_event_loop(_previous); }

    ScopedCurrentEventLoop(ScopedCurrentEventLoop const&)                    = delete;
    auto operator=(ScopedCurrentEventLoop const&) -> ScopedCurrentEventLoop& = delete;

private:

    ThreadCtx* _thread_ctx;
    EventLoop* _previous;
};

} // namespace jb::core::priv
