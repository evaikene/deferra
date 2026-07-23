/** @file system_info_rpc.hpp
 * @brief Defines the public JobU `system.info` JSON-RPC integration seam.
 */
#pragma once

#include "system_info.hpp"

#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

/** Returns the exact public method name registered by register_system_info_method().
 *
 * @return Immutable `system.info` view with process lifetime.
 */
[[nodiscard]] auto system_info_rpc_method_name() noexcept -> std::string_view;

/** Registers the public `system.info` JSON-RPC handler.
 *
 * Registration and handler dispatch are synchronous on the Server event-loop thread. The installed handler owns the
 * moved or copied @p info snapshot, so the caller does not need to retain the supplied SystemInfo.
 *
 * The handler ignores every parameter value dispatched by the Server. This permits future optional parameters without
 * breaking callers or older daemons. If the Server rejects the method, this function returns false without altering any
 * pre-existing registration.
 *
 * @param server RPC server that receives the `system.info` handler.
 * @param info Immutable daemon information snapshot transferred into the installed handler.
 * @return True when the method was registered; false when the server rejected it.
 */
auto register_system_info_method(jb::rpc::Server& server, SystemInfo info) -> bool;

} // namespace jb::jobu
