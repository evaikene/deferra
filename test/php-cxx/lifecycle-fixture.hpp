#pragma once

#include <phpcxx/zend.hpp>

#include <sapi/embed/php_embed.h>

#include <string_view>

namespace phpcxx_test {

// Test-only process owner. The initial request belongs to Embed; subsequent
// requests can be cycled independently. All external Zend owners must die first.
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(Runtime const&)                    = delete;
    auto operator=(Runtime const&) -> Runtime& = delete;

    auto restart_request() -> bool;

private:
    sapi_module_struct _saved_module;
};

// These helpers require the current thread's active request and retain no Zend
// pointers across calls. Failed evaluations clear their exception before return.
auto evaluate_boolean(std::string_view expression) -> bool;
auto exercise_request() -> bool;

// Counter-only query: safe after request/runtime shutdown, on the owning thread.
auto cleanup_balanced() -> bool;

// Requires an active request with no pending exception. The caller must clear
// the injected exception before normal request shutdown.
auto leave_pending_exception() -> void;

} // namespace phpcxx_test
