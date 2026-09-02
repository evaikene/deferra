#pragma once

#include "event_loop_types.hpp"
#include "object_priv.hpp"

namespace jb::core::priv {

struct TimerPrivate : ObjectPrivate {
    TimerHandle handle;
    Duration    interval{Duration::zero()};
    bool        repeating{false};
};

} // namespace jb::core::priv
