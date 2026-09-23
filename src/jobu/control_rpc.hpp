/// @file control_rpc.hpp
/// @brief Registers run control and cron preview JSON-RPC methods.
///
#pragma once

#include <span>
#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

class AttributeRegistry;
class CronEngine;
class ManagementService;
class Scheduler;

/// Returns the exact method names registered by register_control_methods().
/// The view has process lifetime and is suitable for capability assembly.
[[nodiscard]] auto control_rpc_method_names() noexcept -> std::span<std::string_view const>;

/// Registers synchronous owner-thread run control and cron preview handlers.
///
/// The server and all four borrowed collaborators must share an owner thread, and the collaborators must outlive the
/// installed handlers. Run Now retains ManagementService's idempotency and notification behavior. Cancellation goes
/// through Scheduler's fatal gate and may return a requested, still-running result. Neither adapter mutates storage
/// independently. Success results that exceed the configured frame body return `jobu.response.too_large`; a prior
/// committed mutation remains durable and must be reconciled by replay or a later read.
///
/// Registration stops at the first duplicate or rejected method. Discard a partially registered server before
/// listening.
/// @return True only when all four handlers were registered.
auto register_control_methods(jb::rpc::Server&         server,
                              ManagementService&       management,
                              Scheduler&               scheduler,
                              CronEngine const&        cron,
                              AttributeRegistry const& attributes) -> bool;

} // namespace jb::jobu
