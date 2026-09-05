#pragma once

#include "object_priv.hpp"
#include "process.hpp"

namespace jb::core {

/// Extends the single Object-owned allocation; the owner is bound only after Object construction.
struct Process::Private : priv::ObjectPrivate {
    Process*     owner{nullptr};
    ProcessState state{ProcessState::NotRunning};
};

} // namespace jb::core
