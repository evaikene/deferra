/** @file protocol.hpp
 * @brief Defines public JSON-RPC values, handler contracts, and application-error conversion.
 */
#pragma once

#include "error.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace jb::rpc {

/** Marks an explicitly null JSON-RPC request identifier.
 *
 * This differs from an absent request identifier: absence denotes a notification, while this marker denotes a request
 * whose `id` member is JSON null.
 */
struct NullRequestId {
    /// Compares two stateless null request identifiers.
    /// @param other Null request identifier to compare.
    /// @return Always true because the marker carries no state.
    auto operator==(NullRequestId const& other) const -> bool = default;
};

/** Owns a JSON-RPC request identifier.
 *
 * Signed integers, unsigned integers, strings, and explicit JSON null remain distinct. An absent identifier is
 * represented separately with `std::optional<RequestId>` by the private envelope layer.
 */
using RequestId = std::variant<NullRequestId, std::int64_t, std::uint64_t, std::string>;

/// Standard JSON-RPC error codes plus the JobU application-error code.
enum class ErrorCode : std::int64_t { // NOLINT(performance-enum-size) Codes are used as std::int64_t values.
    /// The received JSON text could not be parsed.
    ParseError       = -32700,
    /// The decoded JSON value is not a valid JSON-RPC request.
    InvalidRequest   = -32600,
    /// No handler is registered for the requested method.
    MethodNotFound   = -32601,
    /// The method parameters are invalid for the selected handler.
    InvalidParams    = -32602,
    /// An internal RPC failure prevented completion.
    InternalError    = -32603,
    /// A represented JobU application operation failed.
    ApplicationError = -32000,
};

/// Owns a JSON-RPC error object independently of its wire representation.
struct RpcError {
    /// Numeric JSON-RPC error code; defaults to `ErrorCode::InternalError`.
    std::int64_t                       code{static_cast<std::int64_t>(ErrorCode::InternalError)};
    /// User-safe error message suitable for transmission to the peer.
    std::string                        message;
    /// Optional owned error data; explicit JSON null remains distinct from absence.
    std::optional<jb::core::JsonValue> data;

    /// Compares the numeric code, message, and optional data.
    /// @param other RPC error to compare.
    /// @return True when every field is equal.
    auto operator==(RpcError const& other) const -> bool = default;
};

/// Identifies the operating-system peer associated with an RPC operation when the transport provides credentials.
struct PeerIdentity {
    /// Optional process identifier of the peer.
    std::optional<std::uint64_t> process_id;
    /// Optional user identifier of the peer.
    std::optional<std::uint64_t> user_id;
    /// Optional group identifier of the peer.
    std::optional<std::uint64_t> group_id;
};

/// Carries transport-derived identity and authentication information for one RPC connection.
struct OperationContext {
    /// Operating-system peer identity; fields remain empty when unavailable.
    PeerIdentity               peer;
    /// Optional authenticated application principal associated with the connection.
    std::optional<std::string> authenticated_principal;
};

/// Nonzero server-assigned identifier for a live RPC connection.
using ConnectionId = std::uint64_t;

/// Supplies connection and operation metadata to a method handler.
struct RequestContext {
    /// Identifier of the connection on which the request arrived; zero is the unassigned default.
    ConnectionId     connection_id{0};
    /// Identity and authentication context associated with the connection.
    OperationContext operation;
};

/// Owning success value or represented RPC error returned synchronously by a method handler.
using MethodResult = jb::core::Result<jb::core::JsonValue, RpcError>;

/** Synchronous method callback invoked by the RPC server.
 *
 * The request context and optional parameters are borrowed for the duration of the call. The returned result owns its
 * JSON value or RPC error. Handlers must not retain references to either argument.
 */
using MethodHandler = std::function<MethodResult(RequestContext const&, std::optional<jb::core::JsonValue> const&)>;

/** Converts a project error to the stable JobU application-error representation.
 *
 * The returned error has numeric code `-32000`, preserves the user-safe message, and contains a data object with only
 * the lower-case category and stable error code. `jb::core::Error::detail` is never transmitted.
 *
 * @param error Project-owned error to convert.
 * @return Owning JSON-RPC application error safe to send to a peer.
 */
[[nodiscard]] auto application_error(jb::core::Error const& error) -> RpcError;

} // namespace jb::rpc
