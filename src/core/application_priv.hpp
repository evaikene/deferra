#pragma once

#include "event_thread.hpp"
#include "object_priv.hpp"

#include <memory>

namespace jb::core::priv {

struct ApplicationPrivate : ObjectPrivate {
    int                          argc      = 0;
    char const**                 argv      = nullptr;
    int                          exit_code = 0;
    std::unique_ptr<EventThread> event_thread;
};

} // namespace jb::core::priv
