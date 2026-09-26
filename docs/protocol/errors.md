# Errors and outcomes

Every failed request uses the [JSON-RPC error envelope](transport.md#request-and-response-envelopes).
The server never includes submitted payloads, secret bytes, output chunks, SQL,
or backend diagnostics in generated public errors.

| Error member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `error.code` | integer | 1.0 | JSON-RPC numeric error identity |
| `error.message` | string | 1.0 | Safe human-readable description |
| `error.data` | object | 1.1 | Present for represented JobU operation errors |
| `error.data.category` | string | 1.1 | Stable error class in lowercase |
| `error.data.code` | string | 1.1 | Stable JobU operation identity |

| Numeric code | Meaning | Client action |
| ---: | --- | --- |
| `-32700` | Invalid JSON body | Correct the JSON or framing |
| `-32600` | Invalid JSON-RPC request envelope | Correct the envelope |
| `-32601` | Unknown method | Check `system.info.capabilities` |
| `-32602` | Malformed or extra method params | Correct the params object |
| `-32603` | Internal RPC failure | Inspect daemon health; the connection may close |
| `-32000` | Represented JobU operation failure | Inspect safe `data.category` and `data.code` |

An application error has numeric `-32000`, a safe `message`, and
`data:{"category":"...","code":"..."}`. Category values on the wire are
lowercase, including `invalid_argument`, `not_found`, `conflict`,
`resource_exhausted`, and `unavailable`. The tables below use readable
category names. A terminal transport or protocol failure can arrive without
an operation error and may leave a sent mutation's outcome unknown. Read the
resource or replay only with the original idempotency key and request.

For example, a stale job update can return:

```json
{"jsonrpc":"2.0","id":7,"error":{"code":-32000,"message":"Job revision does not match the expected revision","data":{"category":"conflict","code":"jobu.job.revision_conflict"}}}
```

## Queue, job, and creation errors

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.queue.invalid_name` | InvalidArgument | Queue name violates UTF-8, length, control-character, or reserved-suffix rules. |
| `jobu.queue.invalid_configuration` | InvalidArgument | Weight, capacity, recovery, retention, warning threshold, or defaults are invalid. |
| `jobu.queue.not_found` | NotFound | The selected queue does not exist. |
| `jobu.queue.name_conflict` | Conflict | An active queue already uses the requested name. |
| `jobu.queue.ambiguous_deleted_name` | Conflict | Several deleted queues match a historical name; select by ID. |
| `jobu.queue.state_conflict` | Conflict | The queue state is incompatible with the operation. |
| `jobu.queue.not_suspended` | Conflict | Delete requires a fully suspended queue. |
| `jobu.queue.has_running_attempt` | Conflict | Running work blocks deletion. |
| `jobu.job.invalid_name` | InvalidArgument | Display name violates UTF-8, length, or control-character rules. |
| `jobu.job.invalid_configuration` | InvalidArgument | Definition fields or schedule combination are invalid. |
| `jobu.job.invalid_payload` | InvalidArgument | Runner payload or reference structure is invalid. |
| `jobu.protocol.value_too_large` | ResourceExhausted | Encoded job payload exceeds its 256 KiB bound. |
| `jobu.job.not_found` | NotFound | The selected job does not exist. |
| `jobu.job.deleted` | Conflict | The selected definition is deleted. |
| `jobu.job.revision_conflict` | Conflict | `expected_revision` is stale. |
| `jobu.job.revision_exhausted` | ResourceExhausted | The durable revision cannot advance. |
| `jobu.job.state_conflict` | Conflict | Current state is incompatible with the operation. |
| `jobu.job.not_suspended` | Conflict | Move or delete requires a fully suspended job. |
| `jobu.job.has_running_attempt` | Conflict | Running work blocks deletion. |
| `jobu.job.immutable` | Conflict | A one-time definition cannot be changed after an attempt starts. |
| `jobu.run.schedule_conflict` | Conflict | Its current scheduled occurrence cannot be refreshed. |
| `jobu.idempotency.invalid_key` | InvalidArgument | Key is not 1–128 valid UTF-8 bytes. |
| `jobu.idempotency.conflict` | Conflict | The retained key belongs to different canonical input. |
| `jobu.attribute.unknown` | InvalidArgument | Attribute name is not registered. |
| `jobu.attribute.invalid_scope` | InvalidArgument | Attribute is not accepted in this layer. |
| `jobu.attribute.invalid_type` | InvalidArgument | JSON type does not match the attribute definition. |
| `jobu.attribute.invalid_value` | InvalidArgument | Value or cross-attribute combination is invalid. |
| `jobu.attribute.invalid_document` | InvalidArgument | Attribute document is invalid or exceeds its bound. |

Queue create, job create, and Run Now can use idempotency keys. Replaying the
same key and canonical request returns the original stored result while the
record remains; no mutation is repeated. A new result may already be durable
when a reply is lost or `jobu.response.too_large` is returned.

## Named secrets

Malformed RPC parameter shape, type, or encoding returns JSON-RPC invalid
params (`-32602`) with no application data. Base64 size checks precede syntax
validation: an apparently oversized value returns `jobu.secret.too_large`
even if its base64 is also malformed. Other malformed base64 returns `-32602`.
Secret operation errors use `-32000` and expose only safe `category` and
`code` data.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.secret.invalid_name` | InvalidArgument | The name or list continuation is not a canonical lowercase attribute-style identifier of at most 128 bytes. Names are not normalized. |
| `jobu.secret.too_large` | ResourceExhausted | A set value exceeds 65,536 raw bytes. Empty and arbitrary binary values are accepted. |
| `jobu.storage.invalid_limit` | InvalidArgument | A metadata page limit is outside 1–200. The default is 100. |
| `jobu.secret.not_found` | NotFound | The secret to delete or a name referenced by a new/replacement job definition does not exist. |
| `jobu.secret.in_use` | Conflict | A current definition or nonterminal run snapshot references the secret and prevents deletion. There is no force option. |
| `jobu.service.stopping` | Unavailable | Secret mutations have been stopped by shutdown or a fatal failure. |

Secret set/delete operations commit before a successful reply. If a reply is
lost or a storage failure interrupts the acknowledgement, inspect metadata
before retrying; a repeated set can advance `updated_at`. A fatal storage or
persisted-data failure closes daemon admission. Metadata contains only name
and timestamps, never value bytes, lengths, digests, or previews.

A current job definition or nonterminal run snapshot protects its referenced
secret from deletion. A stale job revision or failed update does not release
those references. Terminal historical snapshots alone do not block deletion.
There is no public secret-value read API.

## History and statistics reads

Malformed RPC params return JSON-RPC invalid params (`-32602`). Validly shaped
requests that fail a service rule return application `-32000` with safe
`data:{category,code}`. No error includes a payload, output bytes, secret value,
stored filters, SQL, or token internals.

For `attempt.get` and `attempt.output`, `attempt_number` must be an integer from
1 through 9,223,372,036,854,775,807 (`INT64_MAX`). An out-of-range RPC value
returns `-32602` without application `data` and leaves the daemon serving.
The equivalent invalid direct-service request returns
`jobu.history.invalid_request`.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.history.invalid_request` | InvalidArgument | A direct history read has an invalid attempt number, limit, channel, or offset. The connection remains usable. |
| `jobu.history.invalid_cursor` | InvalidArgument | A cursor is malformed, unknown, expired, evicted, or belongs to another method. The current request fails; the connection remains usable. Start a new initial query. |
| `jobu.history.cursor_unavailable` | ResourceExhausted | The server could not issue a unique continuation token. The current query fails; the connection remains usable. |
| `jobu.run.not_found` | NotFound | A requested retained run does not exist. |
| `jobu.attempt.not_found` | NotFound | A requested retained attempt does not exist. |
| `jobu.statistics.invalid_request` | InvalidArgument | The resolved window, limit, scope, or grouping is invalid. |
| `jobu.statistics.invalid_cursor` | InvalidArgument | A statistics cursor is unknown, expired, evicted, or belongs to the other statistics scope. Start a new initial query. |
| `jobu.statistics.cursor_unavailable` | ResourceExhausted | The server could not issue a unique statistics continuation token. |
| `jobu.response.too_large` | ResourceExhausted | A result cannot fit the configured RPC response bound. No read data is truncated into a misleading success; a mutation may already have committed. |
| `jobu.service.stopping` | Unavailable | History or statistics read admission has closed during shutdown or fatal failure. |

Cursor lifetime is fixed at five minutes from the initial page. Earlier eviction
or server restart can invalidate it sooner. Ordinary read errors leave the
connection usable; malformed persisted data or fatal storage failure closes
read admission.

## Run controls and cron preview

These operation errors are reachable through
`job.run_now`, `run.cancel`, `schedule.validate`, and `schedule.next` as
application error `-32000`. Its `data` contains only `category` and `code`;
backend detail and submitted payloads are not echoed.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.run.manual_conflict` | Conflict | Run Now's manual barrier or eligible schedule condition is not satisfied. |
| `jobu.idempotency.conflict` | Conflict | A retained key was reused with different canonical Run Now input. |
| `jobu.run.not_found` | NotFound | Cancellation's run ID does not exist. |
| `jobu.run.state_conflict` | Conflict | Cancellation found a run that is already terminal or otherwise cannot be cancelled. |
| `jobu.scheduler.stopping` | Unavailable | Cancellation was rejected after fatal failure or shutdown. |
| `jobu.schedule.invalid_expression` | InvalidArgument | Cron grammar is invalid or unsupported. |
| `jobu.schedule.invalid_timezone` | InvalidArgument | The timezone name is invalid or unavailable. |
| `jobu.schedule.invalid_count` | InvalidArgument | Preview count is outside 1–200. |
| `jobu.schedule.no_future_occurrence` | InvalidArgument | No later occurrence exists in the bounded calendar search. |
| `jobu.schedule.out_of_range` | ResourceExhausted | A requested occurrence cannot be represented. |

Malformed parameter shapes, wrong JSON types, and non-UTC `after` text return
JSON-RPC invalid params (`-32602`) without application `data`. A fatal
cancellation storage failure closes daemon admission.
