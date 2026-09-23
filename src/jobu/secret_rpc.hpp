/// @file secret_rpc.hpp
/// @brief Registers metadata-only secret administration JSON-RPC methods.
#pragma once

#include <span>
#include <string_view>

namespace jb::rpc {
class Server;
}

namespace jb::jobu {

class SecretService;

/// Returns the exact method names registered by register_secret_methods().
/// The view has process lifetime and can be used to assemble daemon capabilities.
[[nodiscard]] auto secret_rpc_method_names() noexcept -> std::span<std::string_view const>;

/// Registers secret.set, secret.list, and secret.delete on a shared owner thread.
///
/// The service must outlive the handlers. Each handler delegates mutation and failure policy to SecretService and
/// returns metadata or null, never value bytes. Successful results obey the server's response size limit. A result
/// too large after a mutation cannot roll back that committed operation. Registration stops at the first failure;
/// discard a partially registered server before listening.
/// @return True only when all three methods were registered.
auto register_secret_methods(jb::rpc::Server& server, SecretService& service) -> bool;

} // namespace jb::jobu
