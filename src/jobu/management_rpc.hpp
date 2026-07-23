/** @file management_rpc.hpp
 * @brief Defines the public JobU management JSON-RPC integration seam.
 */
#pragma once

#include <span>
#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

class AttributeRegistry;
class ManagementService;

/** Returns the exact management method names registered by register_management_methods().
 *
 * The returned view and its strings have process lifetime. Their order is stable for capability reporting but does not
 * affect JSON-RPC dispatch semantics.
 *
 * @return Immutable process-lifetime view of the implemented management method names.
 */
[[nodiscard]] auto management_rpc_method_names() noexcept -> std::span<std::string_view const>;

/** Registers the implemented management JSON-RPC handlers.
 *
 * Registration and all installed handlers run synchronously on the Server event-loop thread. The installed handlers
 * borrow @p service and @p attributes, which must outlive the registrations created by this call. @p attributes must be
 * the registry used to construct @p service so request and response conversion share the same attribute contract.
 *
 * The function stops at the first rejected Server registration. Earlier registrations remain installed; callers must
 * discard the partially configured Server instead of listening with an incomplete capability set.
 *
 * @param server RPC server that receives the management handlers.
 * @param service Synchronous management service invoked by each handler.
 * @param attributes Attribute registry used for strict request and result conversion.
 * @return True when every implemented management method was registered; false after the first rejected registration.
 */
auto register_management_methods(jb::rpc::Server&         server,
                                 ManagementService&       service,
                                 AttributeRegistry const& attributes) -> bool;

} // namespace jb::jobu
