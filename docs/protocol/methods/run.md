# Run control methods

The daemon exposes `run.get`, `run.list`, and `run.cancel` at API 1.3. Inspect
`system.info.capabilities` for the exact registered method inventory.

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `run.get` | Read a full run | 1.3 | `run get` | `get_run()` |
| `run.list` | List run summaries | 1.3 | `run list` | `list_runs()` |
| `run.cancel` | Cancel pending or active work | 1.3 | `run cancel` | `cancel_run()` |

All require object params. [Shared types](../types.md#run-and-attempt-history)
list every run result member and its introduction version.

## Full run view

Run Now returns a full run view. Cancellation embeds the same view in `run`.
The required fields are `id`, `job_id`, `queue_id`, positive `job_revision`,
`origin`, `type`, `state`, `schedule_owned`, `priority`, `planned_at`,
`runnable_at`, nullable `started_at`, nullable `completed_at`, `attributes`,
`payload`, and nullable `result`. Times use canonical UTC RFC 3339 text with
six fractional digits. The attributes are the complete immutable materialized
job set; the payload is the original durable JSON template. Neither resolved
secret bytes nor attempts or captured output are included.

## Get a run (`run.get`)

Params contain exactly `{"run_id":"<canonical UUID>"}`. The result is the full
run view above. Missing IDs return `jobu.run.not_found`. A read never changes
the run or advances the scheduler. A lost reply can be retried as a read.

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `run_id` | ID | Yes | 1.3 | Retained run to read |

## List runs (`run.list`)

An initial params object may contain `queue_id`, `job_id`, `state`, `origin`,
`type`, `planned`, `started`, `completed`, and `limit`. All are optional and
combine with AND. The default limit is 100; accepted limits are 1–200. IDs
refer to the immutable run snapshot, so moved jobs and deleted owners retain
their history. `origin` accepts `scheduled` or `manual`; `submitted` is reserved.
Each time range has optional `from` (inclusive) and `to` (exclusive) UTC
timestamps. When both are present, `from` must precede `to`. A run with no
start or completion time does not match a nonempty corresponding range.

| Params member | Type | Default | Since | Meaning |
| --- | --- | --- | --- | --- |
| `queue_id` | ID | All queues | 1.3 | Captured queue |
| `job_id` | ID | All jobs | 1.3 | Captured definition |
| `state` | `RunState` | All | 1.3 | Current state |
| `origin` | `scheduled` or `manual` | Both | 1.3 | `submitted` is reserved |
| `type` | `cli` or `http` | Both | 1.3 | Runner family |
| `planned` | time range | Unbounded | 1.3 | Planned-time filter |
| `started` | time range | Unbounded | 1.3 | First-start filter |
| `completed` | time range | Unbounded | 1.3 | Terminal-time filter |
| `limit` | integer | 100 | 1.3 | 1–200 items |
| `cursor` | string | — | 1.3 | Sole member in a continuation |

```json
{"queue_id":"00112233-4455-6677-8899-aabbccddeeff","state":"failed","limit":2}
```

The result has `items`, an array of lightweight run summaries, and
`next_cursor`, a string or null. Each summary contains the full view's identity,
origin, type, state, schedule ownership, priority, planned/runnable time, and
nullable start/completion time, but no attributes, payload, result, or output.
Rows are ordered by `(planned_at DESC, id DESC)`. The service may return fewer
than `limit` items to stay within its 512 KiB result budget.
An [empty result page](../examples/run-list.result.json) has `items:[]` and
`next_cursor:null`.

Resume with `{"cursor":"<opaque token>"}` and no other fields. A cursor is
server-owned, expires five minutes after the first page, and may be evicted
earlier or lost on restart. Retrying a still-valid cursor is allowed. Pages
read live retained data; they are not a frozen multi-request snapshot. Missing,
expired, or wrong-method cursors return `jobu.history.invalid_cursor`.

## Cancel a run (`run.cancel`)

The request contains exactly one field, a canonical UUID `run_id`:

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `run_id` | ID | Yes | 1.3 | Run to cancel |

```json
{"run_id":"00112233-4455-6677-8899-aabbccddeeff"}
```

The result is `{"disposition":"completed|requested","run":<full run view>}`.

| Result member | Type | Since | Meaning |
| --- | --- | --- | --- |
| `disposition` | string | 1.3 | `completed` or `requested` |
| `run` | full run object | 1.3 | State observed at cancellation |

`completed` means pending scheduled or retry-waiting work became durably
cancelled before the reply. Any required recurring successor and suspension
drain commit in the same transaction. `requested` means an active executor
accepted cancellation; the returned run is still Running and retains capacity
until its completion is durably recorded. A repeated active cancellation may
return `requested` again. A terminal run returns `jobu.run.state_conflict`.

The call cancels work owned by the run; it does not cancel a JSON-RPC request.
A lost reply
can leave the outcome unknown; inspect the run through `run.get`. A response
too large for the configured body limit returns `jobu.response.too_large`
after any completed mutation remains durable.

Malformed or extra parameters return JSON-RPC invalid params (`-32602`).
Operation failures return application error `-32000` with safe `message` and
`data:{"category":"...","code":"..."}`; private backend details are omitted.
The equivalent CLI and typed-client routes are listed above.

For example, reading a run ID that is not retained can return:

```json
{"jsonrpc":"2.0","id":5,"error":{"code":-32000,"message":"Run was not found","data":{"category":"not_found","code":"jobu.run.not_found"}}}
```
