#pragma once

#include "event_loop.hpp"
#include "object_priv.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace jb::core::priv {

struct EventThreadPrivate : ObjectPrivate {
    std::unique_ptr<EventLoop>   event_loop;
    std::unique_ptr<std::thread> thread;
    std::atomic_bool             started{false};
    std::atomic_bool             finished{false};
    std::atomic<int>             exit_code{0};
};

} // namespace jb::core::priv
