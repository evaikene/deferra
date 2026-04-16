#pragma once

#include <thread>

namespace jb::core {

class EventLoop;

/// Thread context identifying a running thread
class ThreadCtx {
public:

    using id_t = std::thread::id;

    ThreadCtx()  = default;
    ~ThreadCtx() = default;

    /// Returns a pointer to the current thread context
    static auto current() noexcept -> ThreadCtx* { return &s_ctx; }

    /// Returns a value identiying the thread
    auto id() const noexcept -> id_t { return _id; }

    /// Sets the event loop for the current thread context
    /// @param[in] loop Event loop to set
    void set_event_loop(EventLoop* loop) { _event_loop = loop; }

    /// Returns the event loop associated with the current thread context
    /// @return Event loop associated with the current thread context (can be nullptr if not set)
    [[nodiscard]] auto event_loop() const noexcept -> EventLoop* { return _event_loop; }

private:

    static thread_local ThreadCtx s_ctx;
    id_t                          _id         = std::this_thread::get_id();
    EventLoop*                    _event_loop = nullptr;
};

} // namespace jb::core
