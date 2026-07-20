/** @file client.hpp
 * @brief Defines the transport-independent JSON-RPC client API.
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
#include <optional>
#include <string_view>

namespace jb::rpc {

/** Configures resource limits for an RPC client.
 *
 * Every boundary is inclusive. Options are copied when the client is constructed and remain fixed for its lifetime.
 */
struct ClientOptions {
    /// Inbound and outbound framing limits; defaults to 16 KiB of headers and a 1 MiB body.
    FramingLimits framing;
    /// Per-response JSON nesting limits; the default permits 64 nested containers.
    JsonLimits    json;
    /// Maximum entries accepted in one response batch; defaults to 64, while zero rejects every response batch.
    std::size_t   max_batch_entries{64};
    /// Maximum simultaneously pending calls; defaults to 128, while zero rejects every call.
    std::size_t   max_pending_requests{128};
    /// Maximum unacknowledged framed-output bytes; defaults to 2 MiB, while zero permits no request or notification.
    std::size_t   max_queued_output_bytes{2U * 1024U * 1024U};
};

/** Sends and correlates JSON-RPC calls over one borrowed byte-stream device.
 *
 * The client operates synchronously on its event-loop thread. Calls may remain outstanding concurrently and responses
 * may arrive in any order. Public signals are emitted synchronously on that thread; listeners may call back into the
 * client, but must not block, wait, or start nested event processing.
 */
class Client final : public jb::core::Object {
public:
    /** Borrows an already-open device for exclusive RPC use.
     *
     * @param device Open byte stream borrowed for the entire client lifetime.
     * @param options Immutable resource limits copied into the client.
     * @param parent Optional Object that owns this client and supplies its event-loop affinity.
     * @pre The device is open, outlives the client, is accessed exclusively through this client, and has the same
     * non-null event loop as the client.
     * @warning Construct and use the client only from its event-loop thread.
     */
    explicit Client(jb::core::IODevice& device, ClientOptions options = {}, jb::core::Object* parent = nullptr);

    /** Disconnects from the borrowed device and fails any remaining pending calls.
     *
     * The borrowed device is neither closed nor destroyed.
     */
    ~Client() override;

    /** Sends one method call with a generated positive unsigned request identifier.
     *
     * An empty method or primitive params fail with `rpc.invalid_argument`. Object and array params are accepted;
     * absent params omit the member. The call becomes pending only after the device accepts the complete frame. Local
     * validation, pending-limit, JSON-encoding, and outbound-framing failures leave the client usable. Output-limit
     * overflow and short writes are terminal: they emit protocol_error, fail pending calls, and logically close the
     * client. A successful write can produce a synchronous completion signal before this function returns.
     *
     * @param method Case-sensitive method name copied into the request; exact lower-case `rpc.` names are permitted.
     * @param params Optional owning parameters copied into the request.
     * @return Generated request identifier, or a stable local error. Failed never-written calls consume no identifier.
     */
    [[nodiscard]] auto call(std::string_view method, std::optional<JsonValue> params = std::nullopt)
        -> jb::core::Result<RequestId, jb::core::Error>;

    /** Sends one notification without creating pending correlation state.
     *
     * Validation and write failures follow call() semantics. A successful notification never emits a client completion
     * signal and never contributes to pending_request_count().
     *
     * @param method Case-sensitive method name copied into the notification.
     * @param params Optional object or array parameters; primitive values are rejected.
     * @return Success after the complete frame is accepted, or a stable local error.
     */
    [[nodiscard]] auto notify(std::string_view method, std::optional<JsonValue> params = std::nullopt)
        -> jb::core::Result<void, jb::core::Error>;

    /** Forgets one local request correlation without sending a wire message.
     * @param id Exact request identifier to forget; unknown, non-unsigned, and already-completed identifiers are
     * no-ops.
     */
    void cancel(RequestId const& id);

    /** Logically closes the client without closing the borrowed device.
     *
     * The operation is idempotent. Device subscriptions are disconnected and pending calls synchronously emit
     * request_failed with `rpc.connection_closed`. Later calls and notifications fail locally.
     */
    void close();

    /** Reports calls that can still be correlated with peer responses.
     * @return Number of live pending calls; zero after close.
     */
    [[nodiscard]] auto pending_request_count() const noexcept -> std::size_t;

    /** Emitted synchronously after a successful result is correlated.
     *
     * The identifier is removed before emission. A listener may cancel another call, create a new call, or close the
     * client. Closing stops delivery of later entries from the same response batch.
     */
    jb::core::Signal<RequestId, JsonValue> result_received;

    /** Emitted synchronously after a remote JSON-RPC error is correlated.
     *
     * The identifier is removed before emission. A represented remote error is not a client protocol failure.
     */
    jb::core::Signal<RequestId, RpcError> error_received;

    /** Emitted synchronously once for each pending call lost to a terminal local, protocol, device, or close failure.
     *
     * Pending state is cleared before emission and identifiers are reported in ascending generated order. For protocol
     * failures, protocol_error is emitted first. Listeners may reenter cleanup methods without duplicating outcomes.
     */
    jb::core::Signal<RequestId, jb::core::Error> request_failed;

    /** Emitted synchronously once when a peer or output-contract violation terminally closes the logical client.
     *
     * Malformed framing or JSON, invalid or uncorrelatable responses, output overflow, and short writes use this
     * signal. Device failure and explicit close do not. The signal precedes request_failed emissions and may reenter
     * close().
     */
    jb::core::Signal<jb::core::Error> protocol_error;

private:
    /// Owns correlation and stream state without exposing private envelopes or a concrete transport.
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::rpc
