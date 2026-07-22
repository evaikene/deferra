/**
 * @file management_json.hpp
 * @brief Defines transport-independent JSON conversion for JobU management values.
 *
 * Management clients encode typed requests before invoking JSON-RPC and decode typed results after a successful
 * response. Daemon handlers perform the inverse conversions before and after calling ManagementService. Every returned
 * value owns its contents; inputs and registries are borrowed only for the duration of the conversion.
 *
 * Request conversion validates wire structure, member representations, and adapter-owned bounds. Domain rules such as
 * queue/job names, positive configuration and revision values, cron availability, and runner-payload semantics remain
 * ManagementService responsibilities so direct and transported requests share one service policy.
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
 * ManagementService remains responsible for domain constraints such as positive configuration values.
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
 * ManagementService remains responsible for domain constraints such as positive configuration values.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning CreateQueueRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto create_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<CreateQueueRequest, jb::core::Error>;

/** Encodes queue.list parameters.
 *
 * `include_deleted` and `limit` are always emitted. The optional `state` filter and `after_id` pagination cursor are
 * omitted when absent.
 *
 * @param request Typed list request to encode without retaining it.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the request cannot be represented.
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
 * ManagementService remains responsible for domain constraints such as positive configuration values.
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
 * ManagementService remains responsible for domain constraints such as positive configuration values.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning UpdateQueueRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto update_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<UpdateQueueRequest, jb::core::Error>;

/** Encodes a job-definition result using the stable management wire shape.
 *
 * Job attributes are encoded through @p registry at AttributeScope::Job. UUIDs and timestamps use their canonical
 * textual forms, schedules retain their explicit kind, and the payload remains an owning JSON object.
 *
 * @param job Job definition to encode without retaining it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning JSON object, or `jobu.protocol.invalid_response` when the definition cannot be represented.
 */
[[nodiscard]] auto job_to_json(JobDefinition const& job, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes and validates a job-definition result.
 *
 * Every known field is mandatory and validated. Unknown result and nested schedule members are ignored for forward
 * compatibility. Attributes must form a complete materialized Job-scope set according to @p registry.
 *
 * @param value JSON result object to decode without retaining references to it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning JobDefinition, or `jobu.protocol.invalid_response` when a known field is missing or invalid.
 */
[[nodiscard]] auto job_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<JobDefinition, jb::core::Error>;

/** Encodes a bounded job page using the stable management wire shape.
 *
 * Pages may contain at most 200 definitions in strict ascending UUID order. A present cursor must equal the final job
 * ID.
 *
 * @param page Job page to encode without retaining it.
 * @param registry Attribute registry borrowed while encoding each definition.
 * @return Owning JSON object, or `jobu.protocol.invalid_response` when the page cannot be represented.
 */
[[nodiscard]] auto job_page_to_json(JobPage const& page, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes and validates a bounded job page.
 *
 * The known `items` and `next_after_id` fields are mandatory. Unknown page, definition, and nested schedule members
 * are ignored for forward compatibility. At most 200 definitions are accepted in strict ascending UUID order, and a
 * present cursor must equal the final job ID.
 *
 * @param value JSON result object to decode without retaining references to it.
 * @param registry Attribute registry borrowed while decoding each definition.
 * @return Owning JobPage, or `jobu.protocol.invalid_response` when a known field is missing or invalid.
 */
[[nodiscard]] auto job_page_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<JobPage, jb::core::Error>;

/** Encodes job operation parameters containing one definition ID.
 *
 * The result is suitable for job.get, job.suspend, and job.resume.
 *
 * @param id Stable job-definition UUID to encode.
 * @return Owning JSON object containing `job_id`.
 */
[[nodiscard]] auto job_id_to_json(jb::core::Uuid const& id) -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job operation parameters containing one definition ID.
 *
 * The request must contain exactly one canonical `job_id` member and no unknown members.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Job-definition UUID, or `jobu.protocol.invalid_request` when the object is invalid.
 */
[[nodiscard]] auto job_id_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<jb::core::Uuid, jb::core::Error>;

/** Encodes job.create parameters.
 *
 * Job attributes are encoded through @p registry at AttributeScope::Job. Both one-time and reserved cron schedules
 * are representable. The optional name and idempotency key are omitted when absent.
 * ManagementService remains responsible for name, schedule-availability, payload-semantic, size, and idempotency
 * policy.
 *
 * @param request Typed create request to encode without retaining it.
 * @param registry Attribute registry borrowed for this conversion.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the request cannot be represented.
 */
[[nodiscard]] auto create_job_request_to_json(CreateJobRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job.create parameters.
 *
 * Exactly one queue selector, `schedule`, and object-valued `payload` are required. Omitted `type`, `priority`, and
 * `attributes` members use their typed request defaults. Unknown request and nested schedule members are rejected.
 * Cron schedules are structurally accepted for later service-level rejection.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed while decoding partial Job-scope attributes.
 * @return Owning CreateJobRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto create_job_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<CreateJobRequest, jb::core::Error>;

/** Encodes job.list parameters.
 *
 * `include_deleted` and `limit` are always emitted. Optional queue, lifecycle-state, runner-type, and pagination
 * filters are omitted when absent.
 *
 * @param request Typed list request to encode without retaining it.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the request cannot be represented.
 */
[[nodiscard]] auto job_list_request_to_json(JobListRequest const& request)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job.list parameters.
 *
 * The request must be an object with no unknown members and at most one queue selector. Omitted members use the
 * defaults from JobListRequest. Numeric values are range-checked before conversion.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Owning JobListRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto job_list_request_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<JobListRequest, jb::core::Error>;

/** Encodes job.update parameters.
 *
 * Only supplied changes are emitted. A supplied outer name optional with no inner value becomes JSON null, preserving
 * the distinction between an unchanged and cleared name. Attribute changes remain a partial Job-scope patch.
 * ManagementService remains responsible for positive revisions and domain semantics.
 *
 * @param request Typed update request to encode without retaining it.
 * @param registry Attribute registry borrowed while encoding supplied Job-scope changes.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the update is empty or cannot be represented.
 */
[[nodiscard]] auto update_job_request_to_json(UpdateJobRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job.update parameters.
 *
 * `job_id`, a representable `expected_revision`, and at least one effective change are required. Missing `name` leaves
 * it unchanged, JSON null clears it, and a string replaces it. Unknown request and nested schedule members are
 * rejected. Revision positivity and cron availability remain ManagementService policy.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @param registry Attribute registry borrowed while decoding partial Job-scope changes.
 * @return Owning UpdateJobRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto update_job_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<UpdateJobRequest, jb::core::Error>;

/** Encodes job.move parameters.
 *
 * The target queue selector becomes exactly one of `target_queue_id` or `target_queue_name`.
 * ManagementService remains responsible for positive revisions and move-domain policy.
 *
 * @param request Typed move request to encode without retaining it.
 * @return Owning JSON object, or `jobu.protocol.invalid_request` when the request cannot be represented.
 */
[[nodiscard]] auto move_job_request_to_json(MoveJobRequest const& request)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job.move parameters.
 *
 * The request requires `job_id`, a representable `expected_revision`, exactly one target queue selector, and no
 * unknown members. Revision positivity remains ManagementService policy.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Owning MoveJobRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto move_job_request_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<MoveJobRequest, jb::core::Error>;

/** Encodes job.delete parameters.
 *
 * ManagementService remains responsible for positive revisions and deletion-domain policy.
 *
 * @param request Typed delete request to encode without retaining it.
 * @return Owning JSON object containing `job_id` and `expected_revision`.
 */
[[nodiscard]] auto delete_job_request_to_json(DeleteJobRequest const& request)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

/** Decodes job.delete parameters.
 *
 * The request requires `job_id`, a representable `expected_revision`, and no unknown members. Revision positivity
 * remains ManagementService policy.
 *
 * @param value JSON request object to decode without retaining references to it.
 * @return Owning DeleteJobRequest, or `jobu.protocol.invalid_request` when the request shape or value is invalid.
 */
[[nodiscard]] auto delete_job_request_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<DeleteJobRequest, jb::core::Error>;

} // namespace jb::jobu
