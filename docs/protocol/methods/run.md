# Run control methods

The daemon exposes `run.get`, `run.list`, and `run.cancel` at API 1.3. Inspect
`system.info.capabilities` for the exact registered method inventory.

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

## List runs (`run.list`)

An initial params object may contain `queue_id`, `job_id`, `state`, `origin`,
`type`, `planned`, `started`, `completed`, and `limit`. All are optional and
combine with AND. The default limit is 100; accepted limits are 1–200. IDs
refer to the immutable run snapshot, so moved jobs and deleted owners retain
their history. `origin` accepts `scheduled` or `manual`; `submitted` is reserved.
Each time range has optional `from` (inclusive) and `to` (exclusive) UTC
timestamps. When both are present, `from` must precede `to`. A run with no
start or completion time does not match a nonempty corresponding range.

```json
{"queue_id":"00112233-4455-6677-8899-aabbccddeeff","state":"failed","limit":2}
```

The result has `items`, an array of lightweight run summaries, and
`next_cursor`, a string or null. Each summary contains the full view's identity,
origin, type, state, schedule ownership, priority, planned/runnable time, and
nullable start/completion time, but no attributes, payload, result, or output.
Rows are ordered by `(planned_at DESC, id DESC)`. The service may return fewer
than `limit` items to stay within its 512 KiB result budget.

Resume with `{"cursor":"<opaque token>"}` and no other fields. A cursor is
server-owned, expires five minutes after the first page, and may be evicted
earlier or lost on restart. Retrying a still-valid cursor is allowed. Pages
read live retained data; they are not a frozen multi-request snapshot. Missing,
expired, or wrong-method cursors return `jobu.history.invalid_cursor`.

## Cancel a run (`run.cancel`)

The request contains exactly one field, a canonical UUID `run_id`:

```json
{"run_id":"00112233-4455-6677-8899-aabbccddeeff"}
```

The result is `{"disposition":"completed|requested","run":<full run view>}`.
`completed` means pending scheduled or retry-waiting work became durably
cancelled before the reply. Any required recurring successor and suspension
drain commit in the same transaction. `requested` means an active executor
accepted cancellation; the returned run is still Running and retains capacity
until its completion is durably recorded. A repeated active cancellation may
return `requested` again. A terminal run returns `jobu.run.state_conflict`.

The call uses the scheduler's owner-thread cancellation and fatal storage gate.
It never cancels a transport request or edits run rows directly. A lost reply
can leave the outcome unknown; inspect the run through `run.get`. A response
too large for the configured body limit returns `jobu.response.too_large`
after any completed mutation remains durable.

Malformed or extra parameters return JSON-RPC invalid params (`-32602`).
Operation failures return application error `-32000` with safe `message` and
`data:{"category":"...","code":"..."}`; private backend details are omitted.
The CLI and typed client will expose these methods in later Phase 8 stages.
