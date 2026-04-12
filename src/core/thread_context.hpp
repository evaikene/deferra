#pragma once

#include <thread>

namespace df::core {

/// Thread context identifying a running thread
class ThreadCtx {
public:
    
    using id_t = std::thread::id;
    
    ThreadCtx() = default;
    ~ThreadCtx() = default;
    
    /// Returns a pointer to the current thread context
    static auto current() noexcept -> ThreadCtx const*
    {
        return &s_ctx;
    }
    
    /// Returns a value identiying the thread
    auto id() const noexcept -> id_t
    {
        return _id;
    }

private:
    
    static thread_local ThreadCtx s_ctx;
    id_t _id = std::this_thread::get_id();
};

} // namespace df::core
