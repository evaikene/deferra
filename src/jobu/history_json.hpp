/// @file history_json.hpp
/// @brief Strict history request and result codecs, including bounded attempt output.
///
#pragma once

#include "history.hpp"
#include "result.hpp"

namespace jb::jobu {

/// Encodes a run.get request containing one canonical, non-nil run ID.
[[nodiscard]] auto run_get_request_to_json(jb::core::Uuid const& id)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes run.get parameters.
[[nodiscard]] auto run_get_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<jb::core::Uuid, jb::core::Error>;

/// Encodes an attempt.get request containing a canonical run ID and attempt number in [1, maximum_attempt_number].
[[nodiscard]] auto attempt_get_request_to_json(AttemptKey const& key)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes attempt.get parameters, rejecting attempt numbers outside the durable domain.
[[nodiscard]] auto attempt_get_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptKey, jb::core::Error>;

/// Encodes an initial run query or cursor-only continuation as RPC parameters.
/// Rejects invalid limits, ranges, enum values, and the reserved submitted origin.
[[nodiscard]] auto run_list_request_to_json(RunListRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes run.list parameters. Unknown members and fields beside cursor are invalid.
[[nodiscard]] auto run_list_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<RunListRequest, jb::core::Error>;

/// Encodes an initial attempt query or cursor-only continuation as RPC parameters.
[[nodiscard]] auto attempt_list_request_to_json(AttemptListRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes attempt.list parameters. Initial requests require run_id; continuations contain only cursor.
[[nodiscard]] auto attempt_list_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptListRequest, jb::core::Error>;

/// Encodes only lightweight run fields, without attributes, payload, result, or output.
[[nodiscard]] auto run_summary_to_json(RunSummary const& summary)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes required lightweight run fields, allowing unknown response members for forward compatibility.
[[nodiscard]] auto run_summary_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<RunSummary, jb::core::Error>;

/// Encodes a bounded run summary page and nullable continuation token.
[[nodiscard]] auto run_page_to_json(RunPage const& page) -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes a run page while ignoring unknown response members.
[[nodiscard]] auto run_page_from_json(jb::core::JsonValue const& value) -> jb::core::Result<RunPage, jb::core::Error>;

/// Encodes a full retained run view, including materialized attributes and the original payload template.
/// Output and attempts remain separate. The registry is borrowed only during conversion.
[[nodiscard]] auto run_details_to_json(RunDetails const& details, AttributeRegistry const& registry)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Encodes a service-owned run snapshot with the same full view used by history reads.
[[nodiscard]] auto run_details_to_json(JobRun const& run, AttributeRegistry const& registry)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes a full run view. Known fields are required; unknown response members are ignored.
[[nodiscard]] auto run_details_from_json(jb::core::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<RunDetails, jb::core::Error>;

/// Encodes only lightweight attempt fields with a valid durable attempt number, without result or output.
[[nodiscard]] auto attempt_summary_to_json(AttemptSummary const& summary)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes required lightweight attempt fields, rejecting out-of-domain numbers while allowing unknown response
/// members.
[[nodiscard]] auto attempt_summary_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptSummary, jb::core::Error>;

/// Encodes one attempt's summary and nullable safe result; output remains separate.
[[nodiscard]] auto attempt_details_to_json(AttemptDetails const& details)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes an attempt detail while ignoring unknown response members.
[[nodiscard]] auto attempt_details_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptDetails, jb::core::Error>;

/// Encodes a bounded attempt summary page and nullable continuation token.
[[nodiscard]] auto attempt_page_to_json(AttemptPage const& page)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes an attempt page while ignoring unknown response members.
[[nodiscard]] auto attempt_page_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptPage, jb::core::Error>;

/// Encodes a validated output request with a durable attempt number, public channel name, and retained-byte offset.
[[nodiscard]] auto attempt_output_request_to_json(AttemptOutputRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes attempt.output parameters; unknown fields and invalid number/channel/limit/offset values fail.
[[nodiscard]] auto attempt_output_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptOutputRequest, jb::core::Error>;

/// Encodes raw chunk bytes as valid UTF-8 text or canonical padded base64, retaining all availability metadata.
[[nodiscard]] auto attempt_output_chunk_to_json(AttemptOutputChunk const& chunk)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes a complete output result into raw bytes; unknown response fields are ignored for forward compatibility.
[[nodiscard]] auto attempt_output_chunk_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptOutputChunk, jb::core::Error>;

} // namespace jb::jobu
