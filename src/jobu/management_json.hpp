/**
 * @file management_json.hpp
 * @brief Defines transport-independent JSON conversion for JobU management values.
 *
 * Queue clients encode typed requests before invoking JSON-RPC and decode typed results after a successful response.
 * Daemon handlers perform the inverse conversions before and after calling ManagementService. Every returned value
 * owns its contents; inputs and registries are borrowed only for the duration of the conversion.
 */
#pragma once

#include "attribute.hpp"
#include "error.hpp"
#include "json.hpp"
#include "management.hpp"
#include "result.hpp"

namespace jb::jobu {

/** Encodes a queue result using the stable management wire shape.
 *
 * Queue-default attributes are encoded through @p registry at AttributeScope::QueueDefault. UUIDs and timestamps use
 * their canonical textual forms.
 *
 * @param queue Queue value to encode without retaining it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning JSON object, or `jobu.protocol.invalid_response` when the queue cannot be represented.
 */
[[nodiscard]] auto queue_to_json(Queue const& queue, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes and validates a queue result.
 *
 * Every known field is mandatory and validated. Unknown members are ignored for forward compatibility. Queue-default
 * attributes are decoded through @p registry at AttributeScope::QueueDefault.
 *
 * @param value JSON result object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning Queue, or `jobu.protocol.invalid_response` when a known field is missing or invalid.
 */
[[nodiscard]] auto queue_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<Queue, jb::core::Error>;

/** Encodes a bounded queue page using the stable management wire shape.
 *
 * Pages may contain at most 200 queues in strict ascending UUID order. A present cursor must equal the final queue ID.
 *
 * @param page Queue page to encode without retaining it.
 * @param registry Attribute registry borrowed while encoding each queue.
 * @return Owning JSON object, or `jobu.protocol.invalid_response` when the page cannot be represented.
 */
[[nodiscard]] auto queue_page_to_json(QueuePage const& page, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes and validates a bounded queue page.
 *
 * The known `items` and `next_after_id` fields are mandatory. Unknown page and queue members are ignored for forward
 * compatibility. At most 200 queues are accepted in strict ascending UUID order, and a present cursor must equal the
 * final queue ID.
 *
 * @param value JSON result object to decode without retaining references to it.
 * @param registry Attribute registry borrowed while decoding each queue.
 * @return Owning QueuePage, or `jobu.protocol.invalid_response` when a known field is missing or invalid.
 */
[[nodiscard]] auto queue_page_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<QueuePage, jb::core::Error>;

/** Encodes queue operation parameters containing exactly one selector.
 *
 * UUID selectors become `queue_id`; name selectors become `queue_name`. The result is suitable for queue.get,
 * queue.suspend, queue.resume, and queue.delete.
 *
 * @param selector Queue UUID or name to encode without retaining it.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the selector cannot be represented.
 */
[[nodiscard]] auto queue_selector_to_json(QueueSelector const& selector)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes queue operation parameters containing exactly one selector.
 *
 * The request must be an object containing exactly one of `queue_id` or `queue_name` and no unknown members.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Owning QueueSelector, or `jobu.protocol.invalid_request` when the object is ambiguous or invalid.
 */
[[nodiscard]] auto queue_selector_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<QueueSelector, jb::core::Error>;

/** Encodes queue.create parameters.
 *
 * Queue-default attributes are encoded through @p registry at AttributeScope::QueueDefault. The optional idempotency
 * key is omitted when absent; inherited retention is encoded as JSON null.
 *
 * @param request Typed create request to encode without retaining it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the request cannot be represented.
 */
[[nodiscard]] auto create_queue_request_to_json(CreateQueueRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes queue.create parameters.
 *
 * `name` is mandatory. Other fields use their typed request defaults when omitted. Unknown members and explicit null
 * outside `history_retention_seconds` are rejected. The registry is used only while decoding queue defaults.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning CreateQueueRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto create_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<CreateQueueRequest, jb::core::Error>;

/** Encodes queue.list parameters.
 *
 * Optional filters and the pagination cursor are omitted when absent.
 *
 * @param request Typed list request to encode without retaining it.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when a supplied filter cannot be represented.
 */
[[nodiscard]] auto queue_list_request_to_json(QueueListRequest const& request)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes queue.list parameters.
 *
 * The request must be an object with no unknown members. Omitted members use the defaults from QueueListRequest.
 * Numeric values are range-checked before conversion to their C++ representations.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Owning QueueListRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto queue_list_request_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<QueueListRequest, jb::core::Error>;

/** Encodes queue.update parameters.
 *
 * Only supplied mutable fields are emitted. A supplied outer retention optional with no inner value becomes JSON
 * null, preserving the distinction between unchanged retention and inheritance from the daemon policy.
 *
 * @param request Typed update request to encode without retaining it.
 * @param registry Attribute registry borrowed while encoding supplied queue defaults.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the update is empty or cannot be represented.
 */
[[nodiscard]] auto update_queue_request_to_json(UpdateQueueRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes queue.update parameters.
 *
 * Exactly one queue selector and at least one mutable field are required. Unknown members are rejected. Missing
 * `history_retention_seconds` leaves retention unchanged, JSON null selects daemon inheritance, and an integer selects
 * an explicit duration. The registry is used only while decoding supplied queue defaults.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning UpdateQueueRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto update_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<UpdateQueueRequest, jb::core::Error>;

} // namespace jb::jobu
