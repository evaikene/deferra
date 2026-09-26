# Shared protocol types

These are the JSON values used by several [methods](README.md). `Since` names
the JobU API version in which the member first appeared. Every listed result
member is present unless its table says otherwise. Nullable members appear as
JSON `null` when they have no value. Request members are omitted for defaults;
an explicit `null` is accepted only where stated.

## Scalars and vocabulary

| Value | Wire form | Rules |
| --- | --- | --- |
| ID | string | Canonical non-nil UUID, for example `00112233-4455-6677-8899-aabbccddeeff` |
| Time | string | RFC 3339 UTC with trailing `Z`; returned times have six fractional digits |
| Revision | integer | Positive job definition revision; send it as `expected_revision` on guarded mutations |
| Page limit | integer | Usually 1–200, default 100; the method table states exceptions |
| Cursor | string | Opaque server token; use alone in a continuation params object |
| `QueueState` | string | `active`, `suspending`, `suspended`, `deleted` |
| `JobState` | string | `active`, `suspending`, `suspended`, `deleted`, `succeeded`, `failed`, `cancelled` |
| `JobType` | string | `cli`, `http` |
| `RunOrigin` | string | `scheduled`, `manual`; `submitted` is reserved and cannot be sent |
| `RunState` | string | `scheduled`, `running`, `retry_wait`, `succeeded`, `failed`, `interrupted`, `cancelled` |
| `AttemptState` | string | `pending`, `running`, `completed` |
| `AttemptOutcome` | string or null | `succeeded`, `failed`, `interrupted`, `cancelled`; null before completion |
| `RecoveryPolicy` | string | `fail_interrupted`, `retry_interrupted` |

The suffix `_id` always means an ID string, not a display name. A queue
selector contains exactly one of `queue_id` and `queue_name`; a target selector
uses `target_queue_id` or `target_queue_name`. Names are exact and are not
normalized. `queue.get`, queue mutations, and `queue.stats` use selectors;
job definition mutations use `job_id`. A historical name may match multiple
deleted queues; select by ID in that case.

## Schedules and time ranges

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `kind` | string | 1.1 | `once` or `cron` |
| `at` | time or `"now"` | 1.1; `"now"` 1.3 | Required for `once`; `"now"` is accepted only by `job.create` |
| `expression` | string | 1.1 | Required for `cron`; five-field expression or supported alias |
| `timezone` | string | 1.1 | Required for `cron`; IANA timezone name or `UTC` |

The complete forms are `{"kind":"once","at":"2030-01-01T00:00:00Z"}` and
`{"kind":"cron","expression":"@daily","timezone":"UTC"}`. Cron uses
calendar time in its named timezone; returned occurrence times are UTC. The
engine accepts cyclic weekday ranges such as `FRI-MON`. Numeric `0` and `7`
both mean Sunday, but they cannot occur together in one weekday field. The
supported aliases are `@hourly`, `@daily`, and `@weekly`. Weekday wildcard
steps such as `*/2` are rejected; use an explicit range such as
`SUN-SAT/2`. During a timezone overlap the earlier UTC occurrence is used;
during a gap the intended local time shifts forward by the gap length. See
[Cron schedules](../cron.md) for the full user-facing grammar and alias meanings,
and [schedule preview](methods/schedule.md) for the method contract.

The reusable range object has these members:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `from` | time | 1.3 | Optional inclusive lower bound |
| `to` | time | 1.3 | Optional exclusive upper bound |

Both bounds may be omitted. When both are present, `from` must precede `to`.
Run history uses `planned`, `started`, and `completed` ranges. Statistics use
the `planned` range and resolve omitted bounds against the daemon clock.

## Queue and job definition results

Every queue result contains:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `id` | ID | 1.1 | Stable queue identity |
| `name` | string | 1.1 | Exact user-facing name, 1–128 UTF-8 bytes on input |
| `state` | `QueueState` | 1.1 | Current lifecycle state |
| `weight` | integer | 1.1 | Positive scheduling weight |
| `concurrency_limit` | integer | 1.1 | Positive combined running limit |
| `recovery_policy` | `RecoveryPolicy` | 1.1 | Startup treatment of interrupted attempts |
| `defaults` | attribute object | 1.1 | Partial layer applied to jobs created afterward |
| `history_retention_seconds` | integer or null | 1.1 | Null inherits daemon policy; zero is unlimited |
| `runnable_wait_warning_ms` | integer | 1.1 | Configured nonnegative warning threshold |
| `created_at` | time | 1.1 | Creation time |
| `updated_at` | time | 1.1 | Last durable mutation time |
| `deleted_at` | time or null | 1.1 | Soft-deletion time |

Every job definition result contains:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `id` | ID | 1.1 | Stable definition identity |
| `queue_id` | ID | 1.1 | Current owning queue |
| `revision` | integer | 1.1 | Positive optimistic revision |
| `name` | string or null | 1.1 | Optional non-unique display name, at most 256 UTF-8 bytes |
| `state` | `JobState` | 1.1 | Current lifecycle state |
| `type` | `JobType` | 1.1 | Runner family |
| `schedule` | schedule object | 1.1 | Stored concrete once or cron schedule |
| `priority` | integer | 1.1 | Signed 32-bit scheduling priority |
| `attributes` | attribute object | 1.1 | Complete materialized job attributes |
| `payload` | object | 1.1 | Original runner payload or secret-reference template |
| `created_at` | time | 1.1 | Creation time |
| `updated_at` | time | 1.1 | Last durable mutation time |
| `deleted_at` | time or null | 1.1 | Soft-deletion time |

`succeeded`, `failed`, and `cancelled` are durable outcomes of a one-time job definition after its final outstanding
run finishes. A recurring definition cannot have one of these states. A terminal definition has no deletion timestamp;
`deleted` remains the separate soft-deletion state. Run and attempt states describe individual execution history.

Retention and runnable-wait warning settings are stored configuration. The
current daemon does not automatically purge retained history or emit
runnable-wait warnings.

`queue.list` and `job.list` return the corresponding full objects, ordered by
ascending ID bytes. Their page members are:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `items` | array of queue or job objects | 1.1 | At most the requested limit; may be shorter for the response byte budget |
| `next_after_id` | ID or null | 1.1 | Exclusive `after_id` for another page, or null at the end |

## Run and attempt history

`run.list` returns summaries. `run.get`, `job.run_now`, and `run.cancel.run`
return full details. Scheduled-run history is an immutable snapshot of the
definition at run creation; updating or moving today's definition does not
rewrite an earlier run.

| Run member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `id` | ID | 1.3 | Stable run identity |
| `job_id` | ID | 1.3 | Definition that produced the run |
| `queue_id` | ID | 1.3 | Queue captured when the run was created |
| `job_revision` | integer | 1.3 | Captured positive definition revision |
| `origin` | `RunOrigin` | 1.3 | How this run was created |
| `type` | `JobType` | 1.3 | Captured runner family |
| `state` | `RunState` | 1.3 | Current retained lifecycle state |
| `schedule_owned` | boolean | 1.3 | Whether it is the schedule's current occurrence |
| `priority` | integer | 1.3 | Captured signed priority |
| `planned_at` | time | 1.3 | Immutable nominal occurrence time |
| `runnable_at` | time | 1.3 | Earliest eligibility time |
| `started_at` | time or null | 1.3 | First actual start |
| `completed_at` | time or null | 1.3 | Terminal completion |
| `attributes` | attribute object | 1.3 | Full details only; immutable materialized values |
| `payload` | object | 1.3 | Full details only; original template, never resolved bytes |
| `result` | object or null | 1.3 | Full details only; safe terminal summary |

`attempt.list` returns summaries. `attempt.get` adds a safe result. Output is
always requested separately.

| Attempt member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `run_id` | ID | 1.3 | Parent run |
| `attempt_number` | integer | 1.3 | Number within that run, from 1 through 9,223,372,036,854,775,807 (`INT64_MAX`) |
| `due_at` | time | 1.3 | Earliest eligible time |
| `started_at` | time or null | 1.3 | Actual start |
| `completed_at` | time or null | 1.3 | Terminal completion |
| `state` | `AttemptState` | 1.3 | Current state |
| `outcome` | `AttemptOutcome` | 1.3 | Null until a terminal outcome is known |
| `result` | object or null | 1.3 | Detail only; safe terminal result |

Run and attempt history pages share these members:

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `items` | array of run or attempt summaries | 1.3 | Bounded page, never includes output |
| `next_cursor` | string or null | 1.3 | Opaque continuation, or null at the end |

## Output chunk

An [`attempt.output`](methods/attempt.md#read-one-output-chunk-attemptoutput)
result has every member below. Offsets count retained bytes; if capture was
truncated, the retained suffix follows the retained prefix without the omitted
middle bytes.

| Member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `run_id` | ID | 1.3 | Requested run |
| `attempt_number` | integer | 1.3 | Requested attempt, in the same 1 through `INT64_MAX` range |
| `channel` | string | 1.3 | `stdout`/`stderr` for CLI, `body`/`headers` for HTTP |
| `status` | string | 1.3 | `available`, `pending`, `not_captured`, or `lost` |
| `offset` | integer | 1.3 | Requested retained-byte offset |
| `bytes_returned` | integer | 1.3 | Raw byte count in this chunk |
| `next_offset` | integer or null | 1.3 | Offset for another chunk, or null at EOF |
| `retained_bytes` | integer | 1.3 | Total stored bytes for this channel |
| `total_bytes` | integer or null | 1.3 | Observed original size, if trustworthy |
| `omitted_bytes` | integer or null | 1.3 | Missing middle size, if trustworthy |
| `truncated` | boolean | 1.3 | Explicit truncation evidence |
| `capture_lost` | boolean | 1.3 | Explicit capture-loss evidence |
| `encoding` | string | 1.3 | `utf8` for text or `base64` for arbitrary bytes |
| `data` | string | 1.3 | This chunk's text or padded RFC 4648 base64 |

An incomplete attempt reports `pending`. A completed channel with no stored
bytes and no loss evidence reports `not_captured`; explicit loss reports
`lost`, even if some bytes remain. Decode each chunk to bytes before joining
it with another: a UTF-8 character can cross a chunk boundary.

## Statistics page

Both [`system.stats`](methods/system.md#retained-statistics-systemstats) and
[`queue.stats`](methods/queue.md#retained-statistics-queuestats) return this
shape. All members below were introduced in API 1.3.

| Page member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `window` | time range | 1.3 | Resolved `from` and `to` bounds |
| `group_by` | string | 1.3 | `none`, `queue`, `job`, `type`, `origin`, or `state` |
| `groups` | array of group objects | 1.3 | Bounded group page |
| `next_cursor` | string or null | 1.3 | Continuation, or null at the end |
| `measurement` | object | 1.3 | Provenance of measured values |

| Group member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `key` | ID, enum string, or null | 1.3 | Group identity; null for `none` |
| `runs` | object | 1.3 | `total`, fixed `states`, `types`, and `origins` counters |
| `attempts` | object | 1.3 | `total`, fixed `states`, `outcomes`, and `retries` counters |
| `capture` | object | 1.3 | `truncated_attempts`, `lost_attempts` |
| `schedule_lateness_ms` | duration object | 1.3 | First start minus planned time, once per run |
| `execution_wall_duration_ms` | duration object | 1.3 | Completed attempt start/end difference |
| `runnable_wait_ms` | null | 1.3 | Unavailable in the current implementation |

| Nested member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `runs.total` | integer | 1.3 | Retained runs in the group |
| `runs.states` | object | 1.3 | `scheduled`, `running`, `retry_wait`, `succeeded`, `failed`, `interrupted`, `cancelled` counts |
| `runs.types` | object | 1.3 | `cli`, `http` counts |
| `runs.origins` | object | 1.3 | `scheduled`, `manual` counts |
| `attempts.total` | integer | 1.3 | Retained attempts of selected runs |
| `attempts.states` | object | 1.3 | `pending`, `running`, `completed` counts |
| `attempts.outcomes` | object | 1.3 | `succeeded`, `failed`, `interrupted`, `cancelled` counts |
| `attempts.retries` | integer | 1.3 | Retained attempts numbered above one |
| `capture.truncated_attempts` | integer | 1.3 | Attempts with a persisted truncation flag |
| `capture.lost_attempts` | integer | 1.3 | Attempts with a persisted loss flag |
| `schedule_lateness_ms.samples` | integer | 1.3 | Valid sample count |
| `schedule_lateness_ms.average`, `.maximum` | number or null | 1.3 | Milliseconds; null when samples is zero |
| `execution_wall_duration_ms.samples` | integer | 1.3 | Valid sample count |
| `execution_wall_duration_ms.average`, `.maximum` | number or null | 1.3 | Milliseconds; null when samples is zero |
| `measurement.timing` | string | 1.3 | `wall_clock_derived` |
| `measurement.runnable_wait` | string | 1.3 | `unavailable` |
| `measurement.capture` | string | 1.3 | `persisted_output_flags` |

Every fixed counter key is present, including when its value is zero. The
measurement values distinguish wall-clock estimates and persisted capture
flags from an eligible-capacity wait measurement.

## Attributes and defaults

Queue `defaults` are a partial layer applied to subsequently created jobs.
The job's `attributes` result is complete and materialized. On `job.create`,
an omitted attribute inherits the applicable default. On `job.update`, the
supplied object patches named values; omitted names stay unchanged. JSON null
is not an attribute value. The
public JSON representation of a duration is an integer count of
milliseconds. Attribute names and values are validated by the daemon's
standard registry.

| Attribute member | JSON type | Built-in default | Accepted values | Since |
| --- | --- | --- | --- | --- |
| `job.timeout` | integer ms | 120000 | 1 ms–30 days | 1.1 |
| `retry.max_attempts` | integer | 1 | 1–100 | 1.1 |
| `retry.strategy` | string | `fixed` | `fixed`, `exponential` | 1.1 |
| `retry.initial_delay` | integer ms | 0 | 0–24 hours | 1.1 |
| `retry.max_delay` | integer ms | 86400000 | 0–30 days; not below initial delay | 1.1 |
| `retry.multiplier` | number | 2.0 | 1–100 | 1.1 |
| `retry.jitter` | number | 0.0 | 0–1 | 1.1 |
| `retry.mode` | string | `reschedule` | `reschedule`, `blocking` | 1.1 |
| `output.capture` | string | `on_error` | `none`, `on_error`, `always` | 1.1 |
| `output.stdout_limit` | integer bytes | 1048576 | 0–67108864 | 1.1 |
| `output.stderr_limit` | integer bytes | 1048576 | 0–67108864 | 1.1 |
| `output.http_body_limit` | integer bytes | 1048576 | 0–67108864 | 1.1 |
| `output.http_headers_limit` | integer bytes | 65536 | 0–4194304 | 1.1 |
| `http.follow_redirects` | boolean | `false` | Boolean | 1.1 |
| `http.idempotency_key` | boolean | `false` | Boolean | 1.1 |
| `http.max_redirects` | integer | 5 | 0–20; positive when redirects enabled | 1.1 |
| `http.retry_errors` | string array | resolve, connect, TLS handshake, timeout, send, receive | Known HTTP error names | 1.1 |
| `http.retry_statuses` | string array | `408`, `429`, `500-599` | HTTP status selectors | 1.1 |
| `http.tls_verify` | boolean | `true` | Boolean | 1.1 |
| `cli.termination_grace` | integer ms | 5000 | 0–300000 | 1.2 |
| `cli.retry_exit_codes` | string array | `["0-255"]` | Exit-code selectors; at most 64 normalized selectors | 1.2 |

The retry error names include `resolve`, `connect`, `tls_handshake`,
`tls_verification`, `timeout`, `send`, `receive`, `redirect`, and `protocol`.
Output limits bound retained bytes; they are not a redaction mechanism. See
[Named secrets](../secrets.md) for the raw-output exposure boundary.

## CLI and HTTP payloads

The `payload` member of a job definition or run detail is the original JSON
object, including any [secret references](../secrets.md). JobU accepts
additional payload members but only the recognized fields below affect
execution. Both the original serialized template and the resolved payload
must fit 256 KiB. Recognized secret references were introduced in API 1.2.

| CLI payload member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `command` | string | Yes | — | 1.2 | Executable path or name |
| `arguments` | array of strings or references | No | `[]` | 1.2 | Ordered argv values; at most 1024 |
| `working_directory` | string | No | `/` | 1.2 | Absolute working directory |
| `environment` | object of strings, nulls, or references | No | `{}` | 1.2 | Environment additions/removals; at most 256 entries |
| `expected_exit_codes` | array of integers | No | `[0]` | 1.2 | Distinct exit codes 0–255 |

`PATH` and reserved `JOBU_*` environment variables cannot hold references.
An executable name without `/` requires an explicit literal `PATH` value.
Arguments and environment values must become valid UTF-8 without NUL after
secret resolution. The prepared argv/environment request is limited to
256 KiB, including injected JobU metadata.

| HTTP payload member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `url` | string | Yes | — | 1.1 | Request URL |
| `method` | string | No | `GET` | 1.1 | HTTP method |
| `headers` | array of header objects | No | `[]` | 1.1 | Ordered request headers |
| `body` | encoded body or reference | No | Absent | 1.1; reference 1.2 | Request-body bytes |
| `expected_statuses` | string array | No | `["200-299"]` | 1.1 | Successful HTTP status selectors |

| Nested member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `headers[].name` | string | Yes | 1.1 | HTTP header name |
| `headers[].value` | string or reference | Yes | 1.1; reference 1.2 | Header value |
| `headers[].sensitive` | boolean | No | 1.1 | Extra redirect protection; reference values are always sensitive |
| `body.encoding` | string | Encoded body: yes | 1.1 | `utf8` or strict padded `base64` |
| `body.data` | string | Encoded body: yes | 1.1 | Encoded bytes |
| `reference.secret` | string | Reference: yes | 1.2 | Exact named secret, replacing one whole value |

A reference is exactly `{"secret":"service.token"}`. It is accepted only
in CLI argument values, eligible environment values, HTTP header values, and
the whole HTTP body. It is not interpolation. Referenced headers are marked
sensitive even when `sensitive:false` is stored. HTTP bodies may contain
arbitrary bytes; CLI text and HTTP header values still undergo their normal
validation after resolution.
