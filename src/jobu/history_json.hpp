/// @file history_json.hpp
/// @brief Strict history request and result codecs, including bounded attempt output.
///
#pragma once

#include "history.hpp"
#include "result.hpp"

namespace jb::jobu {

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

/// Encodes only lightweight attempt fields, without result or output.
[[nodiscard]] auto attempt_summary_to_json(AttemptSummary const& summary)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes required lightweight attempt fields, allowing unknown response members for forward compatibility.
[[nodiscard]] auto attempt_summary_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptSummary, jb::core::Error>;

/// Encodes a validated output request using the public channel names and retained-byte offsets.
[[nodiscard]] auto attempt_output_request_to_json(AttemptOutputRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Strictly decodes attempt.output parameters; unknown fields and invalid channel/limit/offset values fail.
[[nodiscard]] auto attempt_output_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptOutputRequest, jb::core::Error>;

/// Encodes raw chunk bytes as valid UTF-8 text or canonical padded base64, retaining all availability metadata.
[[nodiscard]] auto attempt_output_chunk_to_json(AttemptOutputChunk const& chunk)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;

/// Decodes a complete output result into raw bytes; unknown response fields are ignored for forward compatibility.
[[nodiscard]] auto attempt_output_chunk_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<AttemptOutputChunk, jb::core::Error>;

} // namespace jb::jobu
