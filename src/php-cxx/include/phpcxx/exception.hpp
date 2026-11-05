#pragma once

#include <phpcxx/error.hpp>
#include <phpcxx/zend.hpp>

namespace php {

/// Whether the current request/thread has a pending PHP exception.
auto has_exception() noexcept -> bool;

/// Return a marker for an already pending exception; requires has_exception().
auto pending_exception() -> Error;

/// Signal a PHP failure, preserving an already pending exception unchanged.
/// Requires an active PHP execution frame; without one Zend may fatally bail out.
/// Type/Value/ArgumentCount use native engine errors. Runtime uses exception_class
/// (which must derive from Exception or Error), or Exception when null.
/// A nonzero argument number requires the active internal function call frame.
/// The diagnostic message must not contain embedded NUL bytes.
/// PendingException never creates another exception. Does not throw C++ exceptions
/// for recoverable failures; PHP bailout remains outside C++ RAII guarantees.
auto raise(Error const& error, zend_class_entry* exception_class = nullptr) -> void;

} // namespace php
