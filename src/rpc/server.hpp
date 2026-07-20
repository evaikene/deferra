/** @file server.hpp
 * @brief Defines the transport-independent JSON-RPC server API.
 */
#pragma once

#include "error.hpp"
#include "framing.hpp"
#include "io_device.hpp"
#include "json.hpp"
#include "object.hpp"
#include "protocol.hpp"
#include "result.hpp"
#include "signal.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace jb::rpc {

/** Configures resource limits for an RPC server and each accepted connection.
 *
 * Every boundary is inclusive. Options are copied when the server is constructed and remain fixed for its lifetime.
 */
struct ServerOptions {
    /// Per-connection framing limits; defaults to 16 KiB of headers and a 1 MiB body.
    FramingLimits framing;
    /// Per-message JSON nesting limits; the default permits 64 nested containers.
    JsonLimits    json;
    /// Maximum accepted entries in one request batch; defaults to 64, while zero rejects every non-empty batch.
    std::size_t   max_batch_entries{64};
    /// Maximum simultaneously live connections; defaults to 128, while zero rejects every connection.
    std::size_t   max_connections{128};
    /// Maximum unacknowledged framed-output bytes per connection; defaults to 2 MiB, while zero permits no reply.
    std::size_t   max_queued_output_bytes{2U * 1024U * 1024U};
};

/** Owns generic byte-stream connections and dispatches JSON-RPC requests.
 *
 * Accepted devices and registered handlers are used synchronously on the server's event-loop thread. Each connection
 * has independent framing and output-accounting state. Handlers must not block, start nested event processing, or
 * retain references to their request arguments.
 */
class Server final : public jb::core::Object {
public:
    /** Constructs an empty server.
     * @param options Immutable resource limits copied into the server.
     * @param parent Optional Object that owns this server and supplies its event-loop affinity.
     */
    explicit Server(ServerOptions options = {}, jb::core::Object* parent = nullptr);

    /** Closes all live connections before releasing server state.
     *
     * Registered methods require no explicit cleanup and are discarded only when the server is destroyed.
     */
    ~Server() override;

    /** Registers a synchronous method handler.
     *
     * Empty names, empty handlers, names beginning with exact lower-case `rpc.`, and duplicate names are rejected.
     * Rejection does not replace an existing handler.
     *
     * @param name Case-sensitive method name to own.
     * @param handler Handler copied into the registry.
     * @return True when a new method was registered; false for invalid or duplicate configuration.
     */
    auto register_method(std::string name, MethodHandler handler) -> bool;

    /** Removes an exact method registration.
     * @param name Case-sensitive method name to remove.
     * @return True when a registration was removed; false when no such method exists.
     */
    auto unregister_method(std::string_view name) -> bool;

    /** Tests for an exact method registration without modifying it.
     * @param name Case-sensitive method name to find.
     * @return True when the method is registered.
     */
    [[nodiscard]] auto has_method(std::string_view name) const noexcept -> bool;

    /** Transfers an already-open generic device into the server.
     *
     * The device must have the same non-null event loop as this server and must not be accessed by another RPC owner
     * afterward. Ownership transfers when this function is entered: failed admission destroys the supplied device.
     * On success the server owns it exclusively until the connection closes. Failures use `rpc.invalid_argument` for
     * a null, closed, or incompatible device and `rpc.connection_limit` for connection or identifier exhaustion.
     * @warning Call this function only from the server's event-loop thread.
     *
     * @param device Exclusively owned, already-open device to attach.
     * @param operation Identity and authentication context copied into every request on this connection.
     * @return A nonzero monotonically allocated connection identifier, or a stable local error.
     */
    [[nodiscard]] auto add_connection(std::unique_ptr<jb::core::IODevice> device, OperationContext operation = {})
        -> jb::core::Result<ConnectionId, jb::core::Error>;

    /** Closes one connection.
     * @param id Connection to close; an unknown or already-retired identifier is a no-op.
     */
    void close_connection(ConnectionId id);

    /** Closes every live connection while preserving method registrations.
     *
     * Repeated calls are safe.
     */
    void close();

    /** Returns the number of logically live connections.
     * @return Current connection count.
     */
    [[nodiscard]] auto connection_count() const noexcept -> std::size_t;

    /** Emitted synchronously after a connection is fully installed.
     *
     * A listener may immediately close the connection. Buffered input is processed only after this signal returns.
     */
    jb::core::Signal<ConnectionId> connection_opened;

    /** Emitted synchronously once after a connection is logically removed.
     *
     * Terminal connection failures emit connection_error first. Ordinary JSON-RPC errors do not emit this signal
     * unless the underlying connection later closes.
     */
    jb::core::Signal<ConnectionId> connection_closed;

    /** Emitted synchronously before connection_closed for a terminal connection failure.
     *
     * Framing, device, short-write, output-framing, output-codec, and output-limit failures are terminal. Parse errors,
     * invalid requests, missing methods, and handler failures are represented on the wire and do not emit this signal.
     */
    jb::core::Signal<ConnectionId, jb::core::Error> connection_error;

private:
    /// Owns implementation state without exposing protocol envelopes or concrete transports in this public header.
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::rpc
